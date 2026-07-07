#pragma once

#include "transport.h"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace multiroom::airplay {

class AirPlayDiscovery {
public:
    void start();
    void stop();
    bool active() const;
    void refresh(std::chrono::milliseconds timeout = std::chrono::milliseconds(750));

    void upsert(OutputDevice device);
    std::vector<OutputDevice> list() const;
    std::optional<OutputDevice> find(const std::string& id) const;

private:
    mutable std::mutex mutex_;
    bool active_ = false;
    std::map<std::string, OutputDevice> devices_;
};

}  // namespace multiroom::airplay
