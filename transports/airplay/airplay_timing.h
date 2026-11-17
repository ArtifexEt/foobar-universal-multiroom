#pragma once

#include <chrono>
#include <cstdint>

namespace multiroom::airplay {

constexpr uint64_t ntp_interval(uint64_t units, uint32_t units_per_second) {
    return ((units / units_per_second) << 32) |
        (((units % units_per_second) << 32) / units_per_second);
}

inline uint64_t current_ntp_timestamp() {
    using namespace std::chrono;
    // All timing responders and stream anchors share one clock. System wall
    // time seeds its epoch once, so wall-clock steps do not move active audio.
    struct ClockReference {
        steady_clock::time_point steady_time;
        uint64_t ntp_time;
    };
    static const auto reference = [] {
        const auto steady_time = steady_clock::now();
        const auto wall_time = system_clock::now().time_since_epoch();
        const auto whole_seconds = floor<seconds>(wall_time);
        const auto fraction = duration_cast<nanoseconds>(wall_time - whole_seconds);
        constexpr uint64_t kUnixToNtpSeconds = 2208988800ULL;
        return ClockReference{
            steady_time,
            ((static_cast<uint64_t>(whole_seconds.count()) + kUnixToNtpSeconds) << 32) |
                ntp_interval(static_cast<uint64_t>(fraction.count()), 1000000000),
        };
    }();
    const auto elapsed = duration_cast<nanoseconds>(steady_clock::now() - reference.steady_time);
    return reference.ntp_time + ntp_interval(static_cast<uint64_t>(elapsed.count()), 1000000000);
}

constexpr uint64_t airplay_presentation_ntp(
    uint64_t group_start_ntp,
    uint64_t stream_timestamp,
    uint32_t sample_rate,
    int offset_ms) {
    const auto packet_ntp = group_start_ntp + ntp_interval(stream_timestamp, sample_rate);
    if (offset_ms < 0) {
        return packet_ntp - ntp_interval(static_cast<uint64_t>(-static_cast<int64_t>(offset_ms)), 1000);
    }
    return packet_ntp + ntp_interval(static_cast<uint64_t>(offset_ms), 1000);
}

}  // namespace multiroom::airplay
