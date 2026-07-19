#include "airplay_transport.h"

#include <algorithm>
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

AirPlayPairingResult AirPlayTransport::pair_output(const std::string& id, const std::string& pin) {
    sync_discovered_outputs();
    const auto resolved_id = resolve_output_id(id);
    const auto output = registry_.find(resolved_id);
    if (!output) {
        throw std::out_of_range("Unknown AirPlay output: " + id);
    }
    if (!output->supports_airplay2) {
        throw std::runtime_error("AirPlay PIN pairing requires an AirPlay 2 output: " + id);
    }
    return sessions_.pair_output(*output, pin);
}

void AirPlayTransport::set_enabled_outputs(const std::vector<std::string>& ids) {
    sync_discovered_outputs();
    registry_.set_enabled_outputs(resolve_output_ids(ids));
    sessions_.prepare_outputs(registry_.list());
    sessions_.close_missing_outputs(registry_.list());
}

void AirPlayTransport::set_output_volume(const std::string& id, int volume) {
    sync_discovered_outputs();
    const auto resolved_id = resolve_output_id(id);
    registry_.set_output_volume(resolved_id, volume);
    sessions_.set_volume(resolved_id, volume);
}

void AirPlayTransport::set_output_offset_ms(const std::string& id, int offset_ms) {
    sync_discovered_outputs();
    registry_.set_output_offset_ms(resolve_output_id(id), offset_ms);
}

void AirPlayTransport::set_playback_metadata(const PlaybackMetadata& metadata) {
    playback_metadata_ = metadata;
    sessions_.set_metadata(metadata);
}

void AirPlayTransport::clear_playback_metadata() {
    playback_metadata_.reset();
    sessions_.clear_metadata();
}

void AirPlayTransport::open_stream(const PcmFormat& format) {
    if (format.sample_rate == 0 || format.channels == 0 || format.bits_per_sample == 0) {
        throw std::invalid_argument("PCM format must have non-zero sample rate, channels, and bit depth.");
    }

    stream_format_ = format;
    stream_open_ = true;
}

void AirPlayTransport::connect_selected_outputs() {
    if (!stream_open_) {
        return;
    }

    sessions_.open_for_outputs(registry_.list(), stream_format_);
    if (playback_metadata_) {
        sessions_.set_metadata(*playback_metadata_);
    } else {
        sessions_.clear_metadata();
    }
}

void AirPlayTransport::write_frames(const void* frames, size_t bytes, uint64_t stream_timestamp) {
    if (!stream_open_) {
        throw std::logic_error("AirPlay stream is not open.");
    }
    if (frames == nullptr && bytes != 0) {
        throw std::invalid_argument("Frame buffer cannot be null when bytes are present.");
    }

    const auto registry_outputs = registry_.list();
    const auto ready_ids = sessions_.ready_output_ids();
    std::vector<OutputDevice> outputs;
    outputs.reserve(registry_outputs.size());

    size_t selected_count = 0;
    for (const auto& output : registry_outputs) {
        if (!output.selected) {
            continue;
        }

        ++selected_count;
        if (ready_ids.find(output.id) != ready_ids.end()) {
            outputs.push_back(output);
        }
    }

    if (selected_count != 0 && outputs.empty()) {
        throw std::runtime_error("No selected AirPlay output has a ready session.");
    }

    const auto packets = scheduler_.schedule(
        outputs,
        {stream_timestamp, bytes, stream_format_.sample_rate, 250});
    for (const auto& packet : packets) {
        sessions_.enqueue(packet, frames, bytes);
    }
}

void AirPlayTransport::flush() {
    sessions_.flush();
}

void AirPlayTransport::stop() {
    sessions_.clear_metadata();
    sessions_.stop();
    stream_open_ = false;
}

void AirPlayTransport::add_discovered_output(OutputDevice device) {
    device.type = OutputType::AirPlay;
    discovery_.upsert(std::move(device));
    sync_discovered_outputs();
}

void AirPlayTransport::set_measured_latency_ms(const std::string& id, int measured_latency_ms) {
    registry_.set_measured_latency_ms(resolve_output_id(id), measured_latency_ms);
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
    auto devices = discovery_.list();
    std::vector<std::string> discovered_ids;
    discovered_ids.reserve(devices.size());
    for (auto device : devices) {
        discovered_ids.push_back(device.id);
        auto existing = registry_.find(device.id);
        if (!existing) {
            for (const auto& alias : device.aliases) {
                existing = registry_.find(alias);
                if (existing) break;
            }
        }
        if (existing) {
            device.selected = existing->selected;
            device.volume = existing->volume;
            device.offset_ms = existing->offset_ms;
            device.measured_latency_ms = existing->measured_latency_ms;
        }

        registry_.upsert(std::move(device));
    }
    registry_.retain(discovered_ids);
}

std::string AirPlayTransport::resolve_output_id(const std::string& id) const {
    for (const auto& output : registry_.list()) {
        if (output.id == id ||
            std::find(output.aliases.begin(), output.aliases.end(), id) != output.aliases.end()) {
            return output.id;
        }
    }
    return id;
}

std::vector<std::string> AirPlayTransport::resolve_output_ids(const std::vector<std::string>& ids) const {
    std::vector<std::string> resolved;
    resolved.reserve(ids.size());
    for (const auto& id : ids) {
        const auto canonical_id = resolve_output_id(id);
        if (std::find(resolved.begin(), resolved.end(), canonical_id) == resolved.end()) {
            resolved.push_back(canonical_id);
        }
    }
    return resolved;
}

}  // namespace multiroom::airplay
