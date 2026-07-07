#include "airplay_session.h"

#include "airplay2_session.h"

#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

namespace multiroom::airplay {

namespace {

void validate_stream_format(const PcmFormat& format) {
    if (format.channels != 2 || format.bits_per_sample != 16) {
        throw std::invalid_argument("AirPlay MVP sessions require stereo 16-bit PCM.");
    }
    if (format.sample_rate != 44100 && format.sample_rate != 48000) {
        throw std::invalid_argument("AirPlay MVP sessions require 44.1 kHz or 48 kHz PCM.");
    }
}

void validate_output_for_stream(const OutputDevice& output) {
    AirPlay2SessionBootstrap::validate_mvp_ready(output);
}

bool same_format(const PcmFormat& left, const PcmFormat& right) {
    return left.sample_rate == right.sample_rate &&
           left.channels == right.channels &&
           left.bits_per_sample == right.bits_per_sample;
}

void update_session_from_output(AirPlaySessionState& session, const OutputDevice& output) {
    session.output_id = output.id;
    session.output_name = output.name;
    session.endpoint_host = output.endpoint_host;
    session.endpoint_port = output.endpoint_port;
}

}  // namespace

AirPlaySessionManager::AirPlaySessionManager(std::shared_ptr<AirPlayControlClient> control_client)
    : control_client_(std::move(control_client)) {
    if (!control_client_) {
        throw std::invalid_argument("AirPlay control client cannot be null.");
    }
}

void AirPlaySessionManager::prepare_outputs(const std::vector<OutputDevice>& outputs) {
    std::lock_guard lock(mutex_);

    for (const auto& output : outputs) {
        if (!output.selected) {
            continue;
        }

        auto& session = sessions_[output.id];
        update_session_from_output(session, output);
        if (session.phase == AirPlaySessionPhase::Failed) {
            session.phase = AirPlaySessionPhase::Closed;
            session.last_error.clear();
        }
    }
}

void AirPlaySessionManager::open_for_outputs(const std::vector<OutputDevice>& outputs, const PcmFormat& format) {
    validate_stream_format(format);

    std::lock_guard lock(mutex_);

    for (const auto& output : outputs) {
        if (!output.selected) {
            continue;
        }

        auto& session = sessions_[output.id];
        const bool can_reuse_ready_session =
            session.phase == AirPlaySessionPhase::Ready &&
            session.open &&
            session.endpoint_host == output.endpoint_host &&
            session.endpoint_port == output.endpoint_port &&
            same_format(session.negotiated_format, format);

        update_session_from_output(session, output);

        try {
            validate_output_for_stream(output);
            if (can_reuse_ready_session) {
                session.last_error.clear();
                continue;
            }

            session.phase = AirPlaySessionPhase::Connecting;
            session.open = false;
            session.negotiated_format = {};
            session.rtsp_session_id.clear();
            session.stream_uri.clear();
            session.rtsp_server.clear();
            session.rtsp_supported_methods.clear();
            session.transport_ports = {};
            session.last_error.clear();
            session.queued_packets.clear();

            const auto negotiated = control_client_->open(output, format);
            session.negotiated_format = format;
            session.rtsp_session_id = negotiated.rtsp_session_id;
            session.stream_uri = negotiated.stream_uri;
            session.rtsp_server = negotiated.server_name;
            session.rtsp_supported_methods = negotiated.supported_methods;
            session.transport_ports = negotiated.ports;
            session.phase = AirPlaySessionPhase::Ready;
            session.open = true;
        } catch (const std::exception& e) {
            session.phase = AirPlaySessionPhase::Failed;
            session.open = false;
            session.negotiated_format = {};
            session.rtsp_session_id.clear();
            session.stream_uri.clear();
            session.rtsp_server.clear();
            session.rtsp_supported_methods.clear();
            session.transport_ports = {};
            session.last_error = e.what();
            session.queued_packets.clear();
            throw;
        }
    }
}

void AirPlaySessionManager::close_missing_outputs(const std::vector<OutputDevice>& outputs) {
    std::lock_guard lock(mutex_);

    std::set<std::string> selected;
    for (const auto& output : outputs) {
        if (output.selected) {
            selected.insert(output.id);
        }
    }

    for (auto& [id, session] : sessions_) {
        if (selected.find(id) == selected.end()) {
            close_control_session(session);
            session.phase = AirPlaySessionPhase::Closed;
            session.open = false;
            session.last_error.clear();
            session.queued_packets.clear();
        }
    }
}

void AirPlaySessionManager::set_volume(const std::string& output_id, int volume) {
    std::lock_guard lock(mutex_);

    const auto it = sessions_.find(output_id);
    if (it == sessions_.end() || it->second.phase != AirPlaySessionPhase::Ready || !it->second.open) {
        return;
    }

    control_client_->set_volume(
        it->second.output_id,
        it->second.rtsp_session_id,
        volume);
}

void AirPlaySessionManager::enqueue(const ScheduledPacket& packet, const void* frames, size_t bytes) {
    std::lock_guard lock(mutex_);

    auto it = sessions_.find(packet.output_id);
    if (it == sessions_.end() || it->second.phase != AirPlaySessionPhase::Ready) {
        throw std::logic_error("Cannot enqueue packet for an AirPlay session that is not ready.");
    }

    control_client_->send_audio(
        it->second.output_id,
        it->second.rtsp_session_id,
        packet,
        frames,
        bytes);
    it->second.queued_packets.push_back(packet);
}

void AirPlaySessionManager::flush() {
    std::lock_guard lock(mutex_);

    for (auto& [_, session] : sessions_) {
        if (session.phase == AirPlaySessionPhase::Ready && session.open) {
            control_client_->flush(session.output_id, session.rtsp_session_id);
        }
        session.queued_packets.clear();
    }
}

void AirPlaySessionManager::stop() {
    std::lock_guard lock(mutex_);

    for (auto& [_, session] : sessions_) {
        close_control_session(session);
        session.phase = AirPlaySessionPhase::Closed;
        session.open = false;
        session.last_error.clear();
        session.queued_packets.clear();
    }
}

std::vector<AirPlaySessionState> AirPlaySessionManager::sessions() const {
    std::lock_guard lock(mutex_);

    std::vector<AirPlaySessionState> result;
    result.reserve(sessions_.size());
    for (const auto& [_, session] : sessions_) {
        result.push_back(session);
    }

    return result;
}

std::vector<ScheduledPacket> AirPlaySessionManager::queued_packets() const {
    std::lock_guard lock(mutex_);

    std::vector<ScheduledPacket> result;
    for (const auto& [_, session] : sessions_) {
        result.insert(result.end(), session.queued_packets.begin(), session.queued_packets.end());
    }

    return result;
}

void AirPlaySessionManager::close_control_session(AirPlaySessionState& session) {
    if (session.open || !session.rtsp_session_id.empty()) {
        control_client_->close(session.output_id, session.rtsp_session_id);
    }
    session.rtsp_session_id.clear();
    session.stream_uri.clear();
    session.rtsp_server.clear();
    session.rtsp_supported_methods.clear();
    session.transport_ports = {};
}

}  // namespace multiroom::airplay
