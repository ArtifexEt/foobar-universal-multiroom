#include "airplay_discovery.h"

#include <stdexcept>
#include <utility>

namespace multiroom::airplay {

void AirPlayDiscovery::start() {
    std::lock_guard lock(mutex_);
    active_ = true;
}

void AirPlayDiscovery::stop() {
    std::lock_guard lock(mutex_);
    active_ = false;
}

bool AirPlayDiscovery::active() const {
    std::lock_guard lock(mutex_);
    return active_;
}

void AirPlayDiscovery::upsert(OutputDevice device) {
    if (device.id.empty()) {
        throw std::invalid_argument("Discovered AirPlay device id cannot be empty.");
    }

    device.type = OutputType::AirPlay;

    std::lock_guard lock(mutex_);
    devices_[device.id] = std::move(device);
}

std::vector<OutputDevice> AirPlayDiscovery::list() const {
    std::lock_guard lock(mutex_);

    std::vector<OutputDevice> result;
    result.reserve(devices_.size());
    for (const auto& [_, device] : devices_) {
        result.push_back(device);
    }

    return result;
}

std::optional<OutputDevice> AirPlayDiscovery::find(const std::string& id) const {
    std::lock_guard lock(mutex_);

    const auto it = devices_.find(id);
    if (it == devices_.end()) {
        return std::nullopt;
    }

    return it->second;
}

}  // namespace multiroom::airplay

