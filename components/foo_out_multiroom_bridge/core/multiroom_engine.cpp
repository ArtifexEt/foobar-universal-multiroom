#include "multiroom_engine.h"

#include <stdexcept>

namespace multiroom {

MultiroomEngine::MultiroomEngine(Transport& transport)
    : transport_(transport) {}

void MultiroomEngine::start_discovery() {
    transport_.start_discovery();
}

void MultiroomEngine::stop_discovery() {
    transport_.stop_discovery();
}

void MultiroomEngine::refresh_discovery(std::chrono::milliseconds timeout) {
    transport_.refresh_discovery(timeout);
}

std::vector<OutputDevice> MultiroomEngine::list_outputs() {
    return transport_.list_outputs();
}

void MultiroomEngine::select_outputs(const std::vector<std::string>& ids) {
    transport_.set_enabled_outputs(ids);
}

void MultiroomEngine::set_output_volume(const std::string& id, int volume) {
    transport_.set_output_volume(id, volume);
}

void MultiroomEngine::set_output_offset_ms(const std::string& id, int offset_ms) {
    transport_.set_output_offset_ms(id, offset_ms);
}

void MultiroomEngine::open_stream(const PcmFormat& format) {
    validate_format(format);
    format_ = format;
    clock_.set_sample_rate(format.sample_rate);
    transport_.open_stream(format);
    stream_open_ = true;
}

uint64_t MultiroomEngine::write_interleaved_pcm(const void* frames, size_t bytes) {
    if (!stream_open_) {
        throw std::logic_error("Multiroom stream is not open.");
    }
    if (frames == nullptr && bytes != 0) {
        throw std::invalid_argument("PCM buffer cannot be null when bytes are present.");
    }

    const auto frame_count = frame_count_from_bytes(format_, bytes);
    const auto timestamp = clock_.advance(frame_count);
    transport_.write_frames(frames, bytes, timestamp);
    return timestamp;
}

void MultiroomEngine::flush() {
    clock_.reset();
    transport_.flush();
}

void MultiroomEngine::stop() {
    stream_open_ = false;
    clock_.reset();
    transport_.stop();
}

uint64_t MultiroomEngine::current_frame() const {
    return clock_.current_frame();
}

bool MultiroomEngine::stream_open() const {
    return stream_open_;
}

uint64_t MultiroomEngine::frame_count_from_bytes(const PcmFormat& format, size_t bytes) {
    validate_format(format);

    const auto bytes_per_sample = format.bits_per_sample / 8;
    const auto bytes_per_frame = static_cast<size_t>(format.channels) * bytes_per_sample;
    if (bytes_per_frame == 0) {
        throw std::invalid_argument("PCM format produces zero bytes per frame.");
    }
    if (bytes % bytes_per_frame != 0) {
        throw std::invalid_argument("PCM byte count must contain whole interleaved frames.");
    }

    return static_cast<uint64_t>(bytes / bytes_per_frame);
}

void MultiroomEngine::validate_format(const PcmFormat& format) {
    if (format.sample_rate == 0 || format.channels == 0 || format.bits_per_sample == 0) {
        throw std::invalid_argument("PCM format must have non-zero sample rate, channels, and bit depth.");
    }
    if (format.bits_per_sample % 8 != 0) {
        throw std::invalid_argument("PCM bit depth must be byte aligned.");
    }
}

}  // namespace multiroom
