#include "stdafx.h"
#include "multiroom_component_state.h"

#include <atomic>
#include <cmath>
#include <limits>

namespace {

static constexpr GUID guid_output = {
    0x8d0f885f, 0xf7d7, 0x4a1f, {0x94, 0x7e, 0xe8, 0x7f, 0x0c, 0x36, 0x7d, 0x6e}};
static constexpr GUID guid_device_selected_speakers = {
    0x41e3b2e5, 0x2d0e, 0x40bf, {0x9d, 0x0f, 0xfa, 0x07, 0x5b, 0x7a, 0x88, 0x11}};

constexpr unsigned kAirPlayBitsPerSample = 16;
constexpr t_size kWritableFramesHint = 352;

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
            MultiroomComponentState::instance().stop_playback();
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
        paused_.store(state);
        SetEvent(wake_event_);
    }

    void volume_set(double value) override { volume_db_.store(value); }

    bool is_progressing() override { return !paused_.load(); }

    pfc::eventHandle_t get_trigger_event() override { return wake_event_; }

private:
    void on_update() override {}

    void write(const audio_chunk& data) override {
        if (paused_.load() || data.is_empty()) {
            return;
        }

        const auto channels = data.get_channel_count();
        const auto samples = data.get_sample_count();
        const auto* input = data.get_data();
        if (channels != 2 || input == nullptr) {
            throw exception_unexpected_audio_format_change();
        }

        const double volume = std::pow(10.0, volume_db_.load() / 20.0);
        pcm_buffer_.clear();
        pcm_buffer_.reserve(static_cast<size_t>(samples) * channels * sizeof(int16_t));

        for (t_size i = 0; i < samples * channels; ++i) {
            append_le16(pcm_buffer_, sample_to_pcm16(static_cast<audio_sample>(input[i] * volume)));
        }

        try {
            MultiroomComponentState::instance().write_playback_pcm(pcm_buffer_.data(), pcm_buffer_.size());
        } catch (const std::exception& e) {
            throw_output_error(e);
        }
        SetEvent(wake_event_);
    }

    t_size can_write_samples() override { return kWritableFramesHint; }

    t_size get_latency_samples() override { return 0; }

    void on_flush() override {
        try {
            MultiroomComponentState::instance().flush_playback();
        } catch (const std::exception& e) {
            throw_output_error(e);
        }
        SetEvent(wake_event_);
    }

    void on_force_play() override { SetEvent(wake_event_); }

    void open(audio_chunk::spec_t const& spec) override {
        if (spec.chanCount != 2 || spec.sampleRate != 44100) {
            throw exception_unexpected_audio_format_change();
        }

        const multiroom::PcmFormat format{
            spec.sampleRate,
            spec.chanCount,
            kAirPlayBitsPerSample,
        };
        try {
            MultiroomComponentState::instance().open_playback_stream(format);
        } catch (const std::exception& e) {
            throw_output_error(e);
        }
        SetEvent(wake_event_);
    }

    GUID device_ = {};
    double buffer_length_ = 0.0;
    bool dither_ = false;
    t_uint32 bit_depth_ = 0;
    std::atomic<bool> paused_{false};
    std::atomic<double> volume_db_{0.0};
    std::vector<uint8_t> pcm_buffer_;
    HANDLE wake_event_ = nullptr;
};

static output_factory_t<MultiroomOutput> g_multiroom_output_factory;
