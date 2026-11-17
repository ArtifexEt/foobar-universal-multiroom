#include "airplay_session.h"

#include "airplay2_session.h"

#include <cstdint>
#include <sstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace multiroom::airplay {

namespace {

constexpr size_t kPacketHistoryLimit = 256;

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

AirPlayPairingResult AirPlaySessionManager::pair_output(const OutputDevice& output, const std::string& pin) {
    return control_client_->pair(output, pin);
}

void AirPlaySessionManager::set_remote_command_handler(
    AirPlayRemoteCommandHandler handler) {
    control_client_->set_remote_command_handler(std::move(handler));
}

void AirPlaySessionManager::prepare_outputs(const std::vector<OutputDevice>& outputs) {
    std::lock_guard lock(mutex_);

    for (const auto& output : outputs) {
        if (cancel_open_requested_.load()) {
            throw std::runtime_error("AirPlay session setup cancelled.");
        }
        if (!output.selected) {
            continue;
        }

        auto& session = sessions_[output.id];
        if (session.open || session.phase == AirPlaySessionPhase::Connecting) {
            session.output_name = output.name;
            continue;
        }
        update_session_from_output(session, output);
        if (session.phase == AirPlaySessionPhase::Failed) {
            session.phase = AirPlaySessionPhase::Closed;
            session.last_error.clear();
        }
    }
}

void AirPlaySessionManager::open_for_outputs(const std::vector<OutputDevice>& outputs, const PcmFormat& format) {
    validate_stream_format(format);

    std::lock_guard lifecycle_lock(lifecycle_mutex_);

    size_t selected_count = 0;
    size_t ready_count = 0;
    std::vector<std::string> errors;

    for (const auto& output : outputs) {
        if (cancel_open_requested_.load()) {
            throw std::runtime_error("AirPlay session setup cancelled.");
        }
        if (!output.selected) {
            continue;
        }
        ++selected_count;

        std::string validation_error;
        try {
            validate_output_for_stream(output);
        } catch (const std::exception& e) {
            validation_error = e.what();
        }

        AirPlaySessionState opening;
        update_session_from_output(opening, output);
        opening.phase = AirPlaySessionPhase::Connecting;
        AirPlaySessionState previous;
        {
            std::lock_guard lock(mutex_);
            auto& session = sessions_[output.id];
            if (validation_error.empty() &&
                session.phase == AirPlaySessionPhase::Ready &&
                session.open &&
                session.endpoint_host == output.endpoint_host &&
                session.endpoint_port == output.endpoint_port &&
                same_format(session.negotiated_format, format)) {
                session.output_name = output.name;
                session.last_error.clear();
                ++ready_count;
                continue;
            }
            previous = std::move(session);
            session = opening;
        }

        bool opened = false;
        try {
            // A receiver's setup must leave ready receivers free to accept PCM.
            if (previous.open || !previous.rtsp_session_id.empty()) {
                control_client_->close(previous.output_id, previous.rtsp_session_id);
            }
            if (!validation_error.empty()) {
                throw std::runtime_error(validation_error);
            }
            if (cancel_open_requested_.load()) {
                throw std::runtime_error("AirPlay session setup cancelled.");
            }

            auto negotiated = control_client_->open(output, format);
            opened = true;
            opening.rtsp_session_id = std::move(negotiated.rtsp_session_id);
            control_client_->set_volume(output.id, opening.rtsp_session_id, output.volume);
            if (cancel_open_requested_.load()) {
                throw std::runtime_error("AirPlay session setup cancelled.");
            }
            opening.negotiated_format = format;
            opening.stream_uri = std::move(negotiated.stream_uri);
            opening.rtsp_server = std::move(negotiated.server_name);
            opening.rtsp_supported_methods = std::move(negotiated.supported_methods);
            opening.transport_ports = negotiated.ports;
            opening.phase = AirPlaySessionPhase::Ready;
            opening.open = true;
            ++ready_count;
        } catch (const std::exception& e) {
            if (opened) {
                control_client_->close(output.id, opening.rtsp_session_id);
                opening.rtsp_session_id.clear();
            }
            opening.phase = AirPlaySessionPhase::Failed;
            opening.last_error = e.what();
            errors.push_back((output.name.empty() ? output.id : output.name) + ": " + e.what());
        }
        {
            std::lock_guard lock(mutex_);
            sessions_.at(output.id) = std::move(opening);
        }
    }

    if (cancel_open_requested_.load()) {
        throw std::runtime_error("AirPlay session setup cancelled.");
    }

    if (selected_count != 0 && ready_count == 0 && !errors.empty()) {
        std::ostringstream message;
        message << "No selected AirPlay sessions could be opened.";
        for (const auto& error : errors) {
            message << " " << error;
        }
        throw std::runtime_error(message.str());
    }
}

void AirPlaySessionManager::close_missing_outputs(const std::vector<OutputDevice>& outputs) {
    std::set<std::string> selected;
    for (const auto& output : outputs) {
        if (output.selected) {
            selected.insert(output.id);
        }
    }

    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    for (const auto& [id, rtsp_session_id] : detach_missing_sessions(selected)) {
        control_client_->close(id, rtsp_session_id);
    }
}

void AirPlaySessionManager::set_volume(const std::string& output_id, int volume) {
    std::string session_output_id;
    std::string rtsp_session_id;
    {
        std::lock_guard lock(mutex_);
        const auto it = sessions_.find(output_id);
        if (it == sessions_.end() || it->second.phase != AirPlaySessionPhase::Ready || !it->second.open) {
            return;
        }
        session_output_id = it->second.output_id;
        rtsp_session_id = it->second.rtsp_session_id;
    }

    control_client_->set_volume(
        session_output_id,
        rtsp_session_id,
        volume);
}

void AirPlaySessionManager::stage_metadata(std::optional<PlaybackMetadata> metadata, bool update_existing_sessions) {
    auto snapshot = metadata ? std::make_shared<const PlaybackMetadata>(std::move(*metadata)) : nullptr;
    std::lock_guard lock(mutex_);
    playback_metadata_ = std::move(snapshot);
    if (update_existing_sessions) ++metadata_revision_;
}

void AirPlaySessionManager::publish_metadata() {
    // Keep metadata sequences ordered while leaving the PCM/session lock free
    // during RTSP I/O. Targets include the exact connection's session ID.
    std::lock_guard publish_lock(metadata_publish_mutex_);
    std::vector<std::pair<std::string, std::string>> targets;
    std::shared_ptr<const PlaybackMetadata> snapshot;
    uint64_t revision;
    {
        std::lock_guard lock(mutex_);
        revision = metadata_revision_;
        snapshot = playback_metadata_;
        for (const auto& [id, session] : sessions_) {
            if (session.phase == AirPlaySessionPhase::Ready && session.open &&
                session.metadata_revision_sent != revision) {
                targets.emplace_back(id, session.rtsp_session_id);
            }
        }
    }

    for (const auto& [id, rtsp_session_id] : targets) {
        if (cancel_open_requested_.load()) break;
        std::string error;
        try {
            if (snapshot) {
                control_client_->set_metadata(id, rtsp_session_id, *snapshot);
            } else {
                control_client_->clear_metadata(id, rtsp_session_id);
            }
        } catch (const std::exception& e) {
            error = std::string("AirPlay metadata update failed: ") + e.what();
        }

        std::lock_guard lock(mutex_);
        const auto it = sessions_.find(id);
        if (it != sessions_.end() && it->second.rtsp_session_id == rtsp_session_id) {
            if (error.empty()) {
                it->second.metadata_revision_sent = revision;
            }
            it->second.last_error = std::move(error);
        }
    }
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
    if (it->second.queued_packets.size() > kPacketHistoryLimit) {
        it->second.queued_packets.pop_front();
    }
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

void AirPlaySessionManager::cancel_pending_open() {
    cancel_open_requested_.store(true);
    control_client_->cancel_pending_open();
}

void AirPlaySessionManager::reset_pending_open_cancel() {
    cancel_open_requested_.store(false);
    control_client_->reset_pending_open_cancel();
}

void AirPlaySessionManager::stop() {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    std::lock_guard publish_lock(metadata_publish_mutex_);

    for (const auto& [id, rtsp_session_id] : detach_missing_sessions({})) {
        try {
            control_client_->clear_metadata(id, rtsp_session_id);
        } catch (const std::exception&) {
            // A receiver which stopped responding still needs teardown.
        }
        control_client_->close(id, rtsp_session_id);
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

std::set<std::string> AirPlaySessionManager::ready_output_ids() const {
    std::lock_guard lock(mutex_);

    std::set<std::string> result;
    for (const auto& [id, session] : sessions_) {
        if (session.phase == AirPlaySessionPhase::Ready && session.open) {
            result.insert(id);
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> AirPlaySessionManager::detach_missing_sessions(
    const std::set<std::string>& selected) {
    std::vector<std::pair<std::string, std::string>> detached;
    std::lock_guard lock(mutex_);
    for (auto& [id, session] : sessions_) {
        if (selected.contains(id)) continue;

        if (session.open || !session.rtsp_session_id.empty()) {
            detached.emplace_back(id, std::move(session.rtsp_session_id));
        }
        AirPlaySessionState closed;
        closed.output_id = std::move(session.output_id);
        closed.output_name = std::move(session.output_name);
        closed.endpoint_host = std::move(session.endpoint_host);
        closed.endpoint_port = session.endpoint_port;
        session = std::move(closed);
    }
    return detached;
}

}  // namespace multiroom::airplay
