#pragma once

#include "../transport.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace multiroom {

struct PacketScheduleRequest {
    uint64_t stream_timestamp = 0;
    size_t bytes = 0;
    uint32_t sample_rate = 48000;
    uint32_t minimum_lead_ms = 250;
    uint64_t group_sync_start_rtp = 0;
    bool group_sync_anchor_valid = false;
};

struct ScheduledPacket {
    std::string output_id;
    uint64_t stream_timestamp = 0;
    uint64_t presentation_timestamp = 0;
    uint64_t earliest_send_timestamp = 0;
    uint64_t lead_frames = 0;
    size_t bytes = 0;
    int volume = 0;
    int offset_ms = 0;
    int measured_latency_ms = 0;
    uint64_t group_sync_start_rtp = 0;
    bool group_sync_anchor_valid = false;
};

class PacketScheduler {
public:
    std::vector<ScheduledPacket> schedule(
        const std::vector<OutputDevice>& outputs,
        const PacketScheduleRequest& request) const;

    static int64_t frames_for_ms(int milliseconds, uint32_t sample_rate);

private:
    static uint64_t apply_offset(uint64_t timestamp, int64_t offset_frames);
};

}  // namespace multiroom
