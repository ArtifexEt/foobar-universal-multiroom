#pragma once

#include "../transport.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace multiroom {

class OutputRegistry {
public:
    void upsert(OutputDevice device);
    std::vector<OutputDevice> list() const;
    std::optional<OutputDevice> find(const std::string& id) const;

    void set_enabled_outputs(const std::vector<std::string>& ids);
    void set_output_volume(const std::string& id, int volume);
    void set_output_offset_ms(const std::string& id, int offset_ms);
    void set_measured_latency_ms(const std::string& id, int measured_latency_ms);

private:
    OutputDevice& require_output(const std::string& id);
    const OutputDevice& require_output(const std::string& id) const;

    mutable std::mutex mutex_;
    std::map<std::string, OutputDevice> outputs_;
};

}  // namespace multiroom

