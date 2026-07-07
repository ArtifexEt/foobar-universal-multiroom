#include "packet_scheduler.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace multiroom {

std::vector<ScheduledPacket> PacketScheduler::schedule(
    const std::vector<OutputDevice>& outputs,
    const PacketScheduleRequest& request) const {
    if (request.sample_rate == 0) {
        throw std::invalid_argument("Sample rate cannot be zero.");
    }

    std::vector<ScheduledPacket> result;
    result.reserve(outputs.size());

    for (const auto& output : outputs) {
        if (!output.selected) {
            continue;
        }

        const auto offset_frames = frames_for_ms(output.offset_ms, request.sample_rate);
        const auto presentation_timestamp = apply_offset(request.stream_timestamp, offset_frames);
        const auto measured_latency_ms = std::max(0, output.measured_latency_ms);
        const auto lead_ms = std::min<int64_t>(
            static_cast<int64_t>(request.minimum_lead_ms) + measured_latency_ms,
            std::numeric_limits<int>::max());
        const auto lead_frames = static_cast<uint64_t>(frames_for_ms(static_cast<int>(lead_ms), request.sample_rate));
        const auto earliest_send_timestamp =
            presentation_timestamp > lead_frames ? presentation_timestamp - lead_frames : 0;

        result.push_back({
            output.id,
            request.stream_timestamp,
            presentation_timestamp,
            earliest_send_timestamp,
            lead_frames,
            request.bytes,
            output.volume,
            output.offset_ms,
            output.measured_latency_ms,
        });
    }

    return result;
}

int64_t PacketScheduler::frames_for_ms(int milliseconds, uint32_t sample_rate) {
    if (sample_rate == 0) {
        throw std::invalid_argument("Sample rate cannot be zero.");
    }

    return static_cast<int64_t>(milliseconds) * static_cast<int64_t>(sample_rate) / 1000;
}

uint64_t PacketScheduler::apply_offset(uint64_t timestamp, int64_t offset_frames) {
    if (offset_frames < 0) {
        const auto positive_offset = static_cast<uint64_t>(-offset_frames);
        return positive_offset > timestamp ? 0 : timestamp - positive_offset;
    }

    const auto positive_offset = static_cast<uint64_t>(offset_frames);
    const auto max_timestamp = std::numeric_limits<uint64_t>::max();
    if (positive_offset > max_timestamp - timestamp) {
        return max_timestamp;
    }

    return timestamp + positive_offset;
}

}  // namespace multiroom
