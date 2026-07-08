#include "airplay_transport.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace multiroom::airplay {

AirPlayTransport::AirPlayTransport(std::shared_ptr<AirPlayControlClient> control_client)
    : sessions_(std::move(control_client)) {}

void AirPlayTransport::start_discovery() {
    discovery_.start();
    sync_discovered_outputs();
    discovery_active_ = true;
}

void AirPlayTransport::stop_discovery() {
    discovery_.stop();
    discovery_active_ = false;
}

void AirPlayTransport::refresh_discovery(std::chrono::milliseconds timeout) {
    if (discovery_active_) {
        discovery_.refresh(timeout);
    }
    sync_discovered_outputs();
}

std::vector<OutputDevice> AirPlayTransport::list_outputs() {
    sync_discovered_outputs();
    return registry_.list();
}

void AirPlayTransport::set_enabled_outputs(const std::vector<std::string>& ids) {
    sync_discovered_outputs();
    registry_.set_enabled_outputs(ids);
    sessions_.prepare_outputs(registry_.list());
    sessions_.close_missing_outputs(registry_.list());
    if (stream_open_) {
        sessions_.open_for_outputs(registry_.list(), stream_format_);
    }
}

void AirPlayTransport::set_output_volume(const std::string& id, int volume) {
    registry_.set_output_volume(id, volume);
    sessions_.set_volume(id, volume);
}

void AirPlayTransport::set_output_offset_ms(const std::string& id, int offset_ms) {
    registry_.set_output_offset_ms(id, offset_ms);
}

void AirPlayTransport::open_stream(const PcmFormat& format) {
    if (format.sample_rate == 0 || format.channels == 0 || format.bits_per_sample == 0) {
        throw std::invalid_argument("PCM format must have non-zero sample rate, channels, and bit depth.");
    }

    stream_format_ = format;
    sessions_.flush();
    sessions_.open_for_outputs(registry_.list(), stream_format_);
    stream_open_ = true;
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
        sessions_.enqueue(packet, frames, bytes);
    }
}

void AirPlayTransport::flush() {
    sessions_.flush();
}

void AirPlayTransport::stop() {
    sessions_.stop();
    stream_open_ = false;
}

void AirPlayTransport::add_discovered_output(OutputDevice device) {
    device.type = OutputType::AirPlay;
    discovery_.upsert(std::move(device));
    sync_discovered_outputs();
}

void AirPlayTransport::set_measured_latency_ms(const std::string& id, int measured_latency_ms) {
    registry_.set_measured_latency_ms(id, measured_latency_ms);
}

bool AirPlayTransport::discovery_active() const {
    return discovery_active_;
}

bool AirPlayTransport::stream_open() const {
    return stream_open_;
}

std::vector<ScheduledPacket> AirPlayTransport::queued_packets() const {
    return sessions_.queued_packets();
}

std::vector<AirPlaySessionState> AirPlayTransport::sessions() const {
    return sessions_.sessions();
}

void AirPlayTransport::sync_discovered_outputs() {
    for (auto device : discovery_.list()) {
        const auto existing = registry_.find(device.id);
        if (existing) {
            device.selected = existing->selected;
            device.volume = existing->volume;
            device.offset_ms = existing->offset_ms;
            device.measured_latency_ms = existing->measured_latency_ms;
        }

        registry_.upsert(std::move(device));
    }
}

}  // namespace multiroom::airplay
