#include "stdafx.h"
#include "multiroom_component_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

static constexpr GUID guid_output = {
    0x8d0f885f, 0xf7d7, 0x4a1f, {0x94, 0x7e, 0xe8, 0x7f, 0x0c, 0x36, 0x7d, 0x6e}};
static constexpr GUID guid_device_selected_speakers = {
    0x41e3b2e5, 0x2d0e, 0x40bf, {0x9d, 0x0f, 0xfa, 0x07, 0x5b, 0x7a, 0x88, 0x11}};

constexpr unsigned kAirPlayBitsPerSample = 16;
constexpr size_t kAirPlayPacketFrames = 352;
constexpr double kMinimumBufferSeconds = 0.25;

using PcmFrame = std::array<int16_t, 2>;

int volume_db_to_percent(double volume_db) {
    if (!std::isfinite(volume_db) || volume_db <= -80.0) {
        return 0;
    }
    return std::clamp(static_cast<int>(std::lround(std::pow(10.0, volume_db / 20.0) * 100.0)), 0, 100);
}

int16_t sample_to_pcm16(audio_sample sample) {
    const double value = std::clamp(static_cast<double>(sample), -1.0, 1.0);
    const long scaled = std::lround(value * 32767.0);
    const long clamped = std::clamp(
        scaled,
        static_cast<long>(std::numeric_limits<int16_t>::min()),
        static_cast<long>(std::numeric_limits<int16_t>::max()));
    return static_cast<int16_t>(clamped);
}

void append_le16(std::vector<uint8_t>& out, int16_t sample) {
    const auto value = static_cast<uint16_t>(sample);
    out.push_back(static_cast<uint8_t>(value & 0x00ffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0x00ffu));
}

[[noreturn]] void throw_output_error(const std::exception& e) {
    pfc::throw_exception_with_message<exception_io_data>(e.what());
}

[[noreturn]] void throw_output_error(const std::string& message) {
    pfc::throw_exception_with_message<exception_io_data>(message.c_str());
}

}  // namespace

class MultiroomOutput : public output_impl {
public:
    MultiroomOutput(const GUID& device, double buffer_length, bool dither, t_uint32 bit_depth)
        : device_(device), buffer_length_(buffer_length), dither_(dither), bit_depth_(bit_depth) {
        wake_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (wake_event_ == nullptr) {
            pfc::throw_exception_with_message<exception_io_data>("CreateEvent failed for multiroom output.");
        }
    }

    ~MultiroomOutput() {
        try {
            stop_stream();
        } catch (...) {
        }
        if (wake_event_ != nullptr) {
            CloseHandle(wake_event_);
        }
    }

    static GUID g_get_guid() { return guid_output; }

    static const char* g_get_name() { return "Universal Multiroom Bridge"; }

    static void g_enum_devices(output_device_enum_callback& callback) {
        const char name[] = "Selected AirPlay speakers";
        callback.on_device(guid_device_selected_speakers, name, sizeof(name) - 1);
    }

    static bool g_advanced_settings_query() { return false; }
    static bool g_needs_bitdepth_config() { return false; }
    static bool g_needs_dither_config() { return false; }
    static bool g_needs_device_list_prefixes() { return false; }
    static bool g_supports_multiple_streams() { return false; }
    static bool g_is_high_latency() { return true; }

    unsigned get_forced_sample_rate() override { return 44100; }
    unsigned get_forced_channel_mask() override { return audio_chunk::channel_config_stereo; }

    void pause(bool state) override {
        {
            std::lock_guard lock(queue_mutex_);
            paused_ = state;
            pacing_reset_ = true;
        }
        queue_changed_.notify_all();
        SetEvent(wake_event_);
    }

    void volume_set(double value) override {
        volume_db_.store(value);
        MultiroomComponentState::instance().set_master_volume_percent(volume_db_to_percent(value));
    }

    bool is_progressing() override {
        std::lock_guard lock(queue_mutex_);
        return started_ && !paused_ && worker_error_.empty();
    }

    pfc::eventHandle_t get_trigger_event() override { return wake_event_; }

private:
    void on_update() override {
        std::string worker_error;
        bool failure_stop_requested = false;
        {
            std::lock_guard lock(queue_mutex_);
            worker_error = worker_error_;
            failure_stop_requested = failure_stop_requested_;
        }
        if (!worker_error.empty() && !failure_stop_requested) {
            throw_output_error(worker_error);
        }
    }

    void write(const audio_chunk& data) override {
        if (data.is_empty()) {
            SetEvent(wake_event_);
            return;
        }

        const auto channels = data.get_channel_count();
        const auto samples = data.get_sample_count();
        const auto* input = data.get_data();
        if (channels != 2 || input == nullptr) {
            throw exception_unexpected_audio_format_change();
        }

        std::string worker_error;
        {
            std::lock_guard lock(queue_mutex_);
            worker_error = worker_error_;
            if (worker_error.empty() && started_ && !stopping_ && !paused_) {
                const size_t available = capacity_frames_ > pcm_queue_.size()
                    ? capacity_frames_ - pcm_queue_.size()
                    : 0;
                const size_t to_copy = std::min(static_cast<size_t>(samples), available);
                for (size_t frame = 0; frame < to_copy; ++frame) {
                    pcm_queue_.push_back({
                        sample_to_pcm16(input[frame * channels]),
                        sample_to_pcm16(input[frame * channels + 1]),
                    });
                }
            }
        }
        if (!worker_error.empty()) {
            throw_output_error(worker_error);
        }

        queue_changed_.notify_one();
        SetEvent(wake_event_);
    }

    t_size can_write_samples() override {
        std::lock_guard lock(queue_mutex_);
        if (!started_ || stopping_ || paused_ || !worker_error_.empty()) return 0;
        return capacity_frames_ > pcm_queue_.size() ? capacity_frames_ - pcm_queue_.size() : 0;
    }

    t_size get_latency_samples() override {
        std::lock_guard lock(queue_mutex_);
        const size_t queued = pcm_queue_.size();
        const size_t in_flight = in_flight_frames_.load();
        const size_t max_latency = static_cast<size_t>(std::numeric_limits<t_size>::max());
        if (queued >= max_latency || in_flight > max_latency - queued) {
            return std::numeric_limits<t_size>::max();
        }
        return static_cast<t_size>(queued + in_flight);
    }

    void on_flush() override {
        {
            std::lock_guard lock(queue_mutex_);
            pcm_queue_.clear();
            force_play_ = false;
            flush_requested_ = true;
            pacing_reset_ = true;
            ++stream_epoch_;
        }
        queue_changed_.notify_all();
        SetEvent(wake_event_);
    }

    void on_force_play() override {
        {
            std::lock_guard lock(queue_mutex_);
            force_play_ = true;
        }
        queue_changed_.notify_one();
        SetEvent(wake_event_);
    }

    void open(audio_chunk::spec_t const& spec) override {
        if (spec.chanCount != 2 || spec.sampleRate != 44100) {
            throw exception_unexpected_audio_format_change();
        }

        stop_stream();

        {
            std::lock_guard lock(queue_mutex_);
            playback_format_ = {
                spec.sampleRate,
                spec.chanCount,
                kAirPlayBitsPerSample,
            };
            sample_rate_ = spec.sampleRate;
            capacity_frames_ = static_cast<size_t>(
                std::ceil(std::max(buffer_length_, kMinimumBufferSeconds) * static_cast<double>(sample_rate_)));
            pcm_queue_.clear();
            worker_error_.clear();
            failure_stop_requested_ = false;
            force_play_ = false;
            flush_requested_ = false;
            pacing_reset_ = true;
            paused_ = false;
            stopping_ = false;
            started_ = true;
            stream_open_ = true;
            ++stream_epoch_;
        }

        try {
            render_thread_ = std::thread(&MultiroomOutput::render_loop, this);
        } catch (...) {
            {
                std::lock_guard lock(queue_mutex_);
                started_ = false;
                stopping_ = true;
                stream_open_ = false;
            }
            try {
                MultiroomComponentState::instance().stop_playback();
            } catch (...) {
            }
            throw;
        }
        SetEvent(wake_event_);
    }

    void render_loop() {
        using Clock = std::chrono::steady_clock;
        auto next_send = Clock::now();

        try {
            multiroom::PcmFormat playback_format;
            {
                std::lock_guard lock(queue_mutex_);
                if (stopping_) return;
                playback_format = playback_format_;
            }

            // Discovery, pairing verification and AirPlay session SETUP can take
            // seconds. output::open() must return immediately so none of that
            // network work can stall foobar's UI/playback-control thread.
            MultiroomComponentState::instance().open_playback_stream(playback_format);

            std::unique_lock lock(queue_mutex_);
            while (!stopping_) {
                queue_changed_.wait(lock, [&] {
                    return stopping_ ||
                        flush_requested_ ||
                        (!paused_ && (pcm_queue_.size() >= kAirPlayPacketFrames || (force_play_ && !pcm_queue_.empty())));
                });
                if (stopping_) break;
                if (flush_requested_) {
                    flush_requested_ = false;
                    pacing_reset_ = true;
                    lock.unlock();
                    {
                        std::lock_guard send_lock(send_mutex_);
                        MultiroomComponentState::instance().flush_playback();
                    }
                    lock.lock();
                    continue;
                }
                if (paused_) continue;

                if (pacing_reset_) {
                    next_send = Clock::now();
                    pacing_reset_ = false;
                }

                const auto epoch = stream_epoch_;
                if (queue_changed_.wait_until(lock, next_send, [&] {
                        return stopping_ || paused_ || stream_epoch_ != epoch || pacing_reset_;
                    })) {
                    continue;
                }

                const size_t queued_frames = pcm_queue_.size();
                const size_t frame_count = std::min(kAirPlayPacketFrames, queued_frames);
                if (frame_count == 0 || (frame_count < kAirPlayPacketFrames && !force_play_)) {
                    continue;
                }

                std::vector<uint8_t> packet;
                packet.reserve(kAirPlayPacketFrames * 2 * sizeof(int16_t));
                for (size_t frame = 0; frame < frame_count; ++frame) {
                    const auto pcm = pcm_queue_.front();
                    pcm_queue_.pop_front();
                    append_le16(packet, pcm[0]);
                    append_le16(packet, pcm[1]);
                }
                if (force_play_ && pcm_queue_.empty()) {
                    force_play_ = false;
                }
                while (packet.size() < kAirPlayPacketFrames * 2 * sizeof(int16_t)) {
                    append_le16(packet, 0);
                    append_le16(packet, 0);
                }
                in_flight_frames_.store(kAirPlayPacketFrames);
                SetEvent(wake_event_);

                const auto packet_duration = std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(
                        static_cast<double>(kAirPlayPacketFrames) / static_cast<double>(sample_rate_)));
                next_send += packet_duration;
                const auto now = Clock::now();
                if (next_send + (packet_duration * 4) < now) {
                    next_send = now;
                }

                lock.unlock();
                {
                    std::lock_guard send_lock(send_mutex_);
                    bool packet_is_current = false;
                    {
                        std::lock_guard state_lock(queue_mutex_);
                        packet_is_current = !stopping_ && stream_epoch_ == epoch;
                    }
                    if (packet_is_current) {
                        MultiroomComponentState::instance().write_playback_pcm(packet.data(), packet.size());
                    }
                }
                in_flight_frames_.store(0);
                lock.lock();
            }
        } catch (const std::exception& e) {
            bool cancelled = false;
            {
                std::lock_guard lock(queue_mutex_);
                cancelled = stopping_;
                if (!cancelled) {
                    worker_error_ = e.what();
                    failure_stop_requested_ = true;
                }
                started_ = false;
            }
            if (!cancelled) {
                MultiroomComponentState::instance().report_playback_failure(e.what());
            }
        } catch (...) {
            const std::string message = "Unknown AirPlay render thread failure.";
            {
                std::lock_guard lock(queue_mutex_);
                worker_error_ = message;
                failure_stop_requested_ = true;
                started_ = false;
            }
            MultiroomComponentState::instance().report_playback_failure(message);
        }

        in_flight_frames_.store(0);
        SetEvent(wake_event_);
    }

    void stop_stream() {
        bool stop_transport = false;
        {
            std::lock_guard lock(queue_mutex_);
            stop_transport = stream_open_;
            stopping_ = true;
            started_ = false;
            stream_open_ = false;
            pcm_queue_.clear();
            worker_error_.clear();
            force_play_ = false;
            flush_requested_ = false;
            pacing_reset_ = true;
            failure_stop_requested_ = false;
            ++stream_epoch_;
        }
        queue_changed_.notify_all();
        SetEvent(wake_event_);

        if (render_thread_.joinable()) {
            MultiroomComponentState::instance().cancel_pending_playback_open();
            render_thread_.join();
        }
        in_flight_frames_.store(0);

        if (stop_transport) {
            std::lock_guard send_lock(send_mutex_);
            MultiroomComponentState::instance().stop_playback();
        }
    }

    GUID device_ = {};
    double buffer_length_ = 0.0;
    bool dither_ = false;
    t_uint32 bit_depth_ = 0;
    std::atomic<double> volume_db_{0.0};
    std::atomic<size_t> in_flight_frames_{0};
    std::mutex queue_mutex_;
    std::mutex send_mutex_;
    std::condition_variable queue_changed_;
    std::deque<PcmFrame> pcm_queue_;
    std::thread render_thread_;
    std::string worker_error_;
    size_t capacity_frames_ = 44100;
    uint32_t sample_rate_ = 44100;
    multiroom::PcmFormat playback_format_{44100, 2, kAirPlayBitsPerSample};
    uint64_t stream_epoch_ = 0;
    bool paused_ = false;
    bool stopping_ = true;
    bool started_ = false;
    bool stream_open_ = false;
    bool force_play_ = false;
    bool flush_requested_ = false;
    bool pacing_reset_ = true;
    bool failure_stop_requested_ = false;
    HANDLE wake_event_ = nullptr;
};

static output_factory_t<MultiroomOutput> g_multiroom_output_factory;
