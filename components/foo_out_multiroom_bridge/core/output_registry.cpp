#include "output_registry.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace multiroom {

namespace {

int clamp_volume(int volume) {
    return std::clamp(volume, 0, 100);
}

int clamp_offset_ms(int offset_ms) {
    return std::clamp(offset_ms, -2000, 2000);
}

}  // namespace

void OutputRegistry::upsert(OutputDevice device) {
    if (device.id.empty()) {
        throw std::invalid_argument("Output device id cannot be empty.");
    }

    device.volume = clamp_volume(device.volume);
    device.offset_ms = clamp_offset_ms(device.offset_ms);

    std::lock_guard lock(mutex_);
    outputs_[device.id] = std::move(device);
}

std::vector<OutputDevice> OutputRegistry::list() const {
    std::lock_guard lock(mutex_);

    std::vector<OutputDevice> result;
    result.reserve(outputs_.size());
    for (const auto& [_, output] : outputs_) {
        result.push_back(output);
    }

    return result;
}

std::optional<OutputDevice> OutputRegistry::find(const std::string& id) const {
    std::lock_guard lock(mutex_);

    const auto it = outputs_.find(id);
    if (it == outputs_.end()) {
        return std::nullopt;
    }

    return it->second;
}

void OutputRegistry::set_enabled_outputs(const std::vector<std::string>& ids) {
    std::lock_guard lock(mutex_);

    const std::set<std::string> selected(ids.begin(), ids.end());
    for (const auto& id : selected) {
        require_output(id);
    }

    for (auto& [id, output] : outputs_) {
        output.selected = selected.find(id) != selected.end();
    }
}

void OutputRegistry::set_output_volume(const std::string& id, int volume) {
    std::lock_guard lock(mutex_);
    require_output(id).volume = clamp_volume(volume);
}

void OutputRegistry::set_output_offset_ms(const std::string& id, int offset_ms) {
    std::lock_guard lock(mutex_);
    require_output(id).offset_ms = clamp_offset_ms(offset_ms);
}

void OutputRegistry::set_measured_latency_ms(const std::string& id, int measured_latency_ms) {
    std::lock_guard lock(mutex_);
    require_output(id).measured_latency_ms = std::max(0, measured_latency_ms);
}

OutputDevice& OutputRegistry::require_output(const std::string& id) {
    const auto it = outputs_.find(id);
    if (it == outputs_.end()) {
        throw std::out_of_range("Unknown output device: " + id);
    }

    return it->second;
}

const OutputDevice& OutputRegistry::require_output(const std::string& id) const {
    const auto it = outputs_.find(id);
    if (it == outputs_.end()) {
        throw std::out_of_range("Unknown output device: " + id);
    }

    return it->second;
}

}  // namespace multiroom
