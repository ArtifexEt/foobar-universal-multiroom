#include "sync_clock.h"

#include <algorithm>
#include <stdexcept>

namespace multiroom {

SyncClock::SyncClock(uint32_t sample_rate)
    : sample_rate_(sample_rate) {
    if (sample_rate_ == 0) {
        throw std::invalid_argument("Sample rate cannot be zero.");
    }
}

void SyncClock::reset(uint64_t start_frame) {
    frame_cursor_ = start_frame;
}

void SyncClock::set_sample_rate(uint32_t sample_rate) {
    if (sample_rate == 0) {
        throw std::invalid_argument("Sample rate cannot be zero.");
    }

    sample_rate_ = sample_rate;
    frame_cursor_ = 0;
}

uint32_t SyncClock::sample_rate() const {
    return sample_rate_;
}

uint64_t SyncClock::current_frame() const {
    return frame_cursor_;
}

uint64_t SyncClock::timestamp_for_next_frame() const {
    return frame_cursor_;
}

uint64_t SyncClock::timestamp_after_frames(uint64_t frame_count) const {
    return frame_cursor_ + frame_count;
}

uint64_t SyncClock::advance(uint64_t frame_count) {
    const auto timestamp = frame_cursor_;
    frame_cursor_ += frame_count;
    return timestamp;
}

int64_t SyncClock::frames_for_offset_ms(int offset_ms) const {
    const auto clamped_offset_ms = std::clamp(offset_ms, -2000, 2000);
    return static_cast<int64_t>(clamped_offset_ms) * static_cast<int64_t>(sample_rate_) / 1000;
}

}  // namespace multiroom

