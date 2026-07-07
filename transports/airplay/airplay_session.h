#pragma once

#include "core/packet_scheduler.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace multiroom::airplay {

struct AirPlaySessionState {
    std::string output_id;
    bool open = false;
    std::vector<ScheduledPacket> queued_packets;
};

class AirPlaySessionManager {
public:
    void open_for_outputs(const std::vector<OutputDevice>& outputs);
    void close_missing_outputs(const std::vector<OutputDevice>& outputs);
    void enqueue(const ScheduledPacket& packet);
    void flush();
    void stop();

    std::vector<AirPlaySessionState> sessions() const;
    std::vector<ScheduledPacket> queued_packets() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, AirPlaySessionState> sessions_;
};

}  // namespace multiroom::airplay
