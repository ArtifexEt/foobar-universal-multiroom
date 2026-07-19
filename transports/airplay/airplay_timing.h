#pragma once

#include <chrono>
#include <cstdint>

namespace multiroom::airplay {

inline uint64_t current_ntp_timestamp() {
    using namespace std::chrono;
    constexpr uint64_t kUnixToNtpSeconds = 2208988800ULL;
    const auto now = system_clock::now().time_since_epoch();
    const auto seconds = duration_cast<std::chrono::seconds>(now);
    const auto fraction_ns = duration_cast<std::chrono::nanoseconds>(now - seconds).count();
    const auto fraction = (static_cast<unsigned long long>(fraction_ns) << 32) / 1000000000ULL;
    return ((static_cast<uint64_t>(seconds.count()) + kUnixToNtpSeconds) << 32) | fraction;
}

inline uint64_t ntp_to_rtp_timestamp(uint64_t ntp, uint32_t sample_rate) {
    return ((ntp >> 16) * sample_rate) >> 16;
}

inline uint64_t rtp_timestamp_to_ntp(uint64_t rtp_timestamp, uint32_t sample_rate) {
    return ((rtp_timestamp << 16) / sample_rate) << 16;
}

}  // namespace multiroom::airplay
