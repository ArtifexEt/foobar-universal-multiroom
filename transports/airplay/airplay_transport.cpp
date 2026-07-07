#include "airplay_transport.h"

#include <stdexcept>
#include <utility>

namespace multiroom::airplay {

void AirPlayTransport::start_discovery() {
    discovery_active_ = true;
}

void AirPlayTransport::stop_discovery() {
    discovery_active_ = false;
}

std::vector<OutputDevice> AirPlayTransport::list_outputs() {
    return registry_.list();
}

void AirPlayTransport::set_enabled_outputs(const std::vector<std::string>& ids) {
    registry_.set_enabled_outputs(ids);
}

void AirPlayTransport::set_output_volume(const std::string& id, int volume) {
    registry_.set_output_volume(id, volume);
}

void AirPlayTransport::set_output_offset_ms(const std::string& id, int offset_ms) {
    registry_.set_output_offset_ms(id, offset_ms);
}

void AirPlayTransport::open_stream(const PcmFormat& format) {
    if (format.sample_rate == 0 || format.channels == 0 || format.bits_per_sample == 0) {
        throw std::invalid_argument("PCM format must have non-zero sample rate, channels, and bit depth.");
    }

    stream_format_ = format;
    stream_open_ = true;
    queued_packets_.clear();
}

void AirPlayTransport::write_frames(const void* frames, size_t bytes, uint64_t stream_timestamp) {
    if (!stream_open_) {
        throw std::logic_error("AirPlay stream is not open.");
    }
    if (frames == nullptr && bytes != 0) {
        throw std::invalid_argument("Frame buffer cannot be null when bytes are present.");
    }

    const auto packets = scheduler_.schedule(
        registry_.list(),
        {stream_timestamp, bytes, stream_format_.sample_rate, 250});
    for (const auto& packet : packets) {
        queued_packets_.push_back(packet);
    }
}

void AirPlayTransport::flush() {
    queued_packets_.clear();
}

void AirPlayTransport::stop() {
    flush();
    stream_open_ = false;
}

void AirPlayTransport::add_discovered_output(OutputDevice device) {
    device.type = OutputType::AirPlay;
    registry_.upsert(std::move(device));
}

bool AirPlayTransport::discovery_active() const {
    return discovery_active_;
}

bool AirPlayTransport::stream_open() const {
    return stream_open_;
}

const std::vector<ScheduledPacket>& AirPlayTransport::queued_packets() const {
    return queued_packets_;
}

}  // namespace multiroom::airplay
