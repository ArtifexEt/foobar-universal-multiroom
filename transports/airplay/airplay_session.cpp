#include "airplay_session.h"

#include <set>
#include <stdexcept>
#include <utility>

namespace multiroom::airplay {

void AirPlaySessionManager::open_for_outputs(const std::vector<OutputDevice>& outputs) {
    std::lock_guard lock(mutex_);

    for (const auto& output : outputs) {
        if (!output.selected) {
            continue;
        }

        auto& session = sessions_[output.id];
        session.output_id = output.id;
        session.open = true;
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
        if (!selected.contains(id)) {
            session.open = false;
            session.queued_packets.clear();
        }
    }
}

void AirPlaySessionManager::enqueue(const ScheduledPacket& packet) {
    std::lock_guard lock(mutex_);

    auto it = sessions_.find(packet.output_id);
    if (it == sessions_.end() || !it->second.open) {
        throw std::logic_error("Cannot enqueue packet for a closed AirPlay session.");
    }

    it->second.queued_packets.push_back(packet);
}

void AirPlaySessionManager::flush() {
    std::lock_guard lock(mutex_);

    for (auto& [_, session] : sessions_) {
        session.queued_packets.clear();
    }
}

void AirPlaySessionManager::stop() {
    std::lock_guard lock(mutex_);

    for (auto& [_, session] : sessions_) {
        session.open = false;
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

}  // namespace multiroom::airplay

