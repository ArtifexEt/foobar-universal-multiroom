#pragma once

#include <cstdint>

namespace multiroom {

class SyncClock {
public:
    explicit SyncClock(uint32_t sample_rate = 48000);

    void reset(uint64_t start_frame = 0);
    void set_sample_rate(uint32_t sample_rate);

    uint32_t sample_rate() const;
    uint64_t current_frame() const;
    uint64_t timestamp_for_next_frame() const;
    uint64_t timestamp_after_frames(uint64_t frame_count) const;
    uint64_t advance(uint64_t frame_count);
    int64_t frames_for_offset_ms(int offset_ms) const;

private:
    uint32_t sample_rate_ = 48000;
    uint64_t frame_cursor_ = 0;
};

}  // namespace multiroom

