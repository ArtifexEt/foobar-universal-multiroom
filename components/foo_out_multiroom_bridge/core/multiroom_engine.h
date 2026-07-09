#pragma once

#include "../transport.h"
#include "sync_clock.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace multiroom {

class MultiroomEngine {
public:
    explicit MultiroomEngine(Transport& transport);

    void start_discovery();
    void stop_discovery();
    void refresh_discovery(std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));
    std::vector<OutputDevice> list_outputs();

    void select_outputs(const std::vector<std::string>& ids);
    void set_output_volume(const std::string& id, int volume);
    void set_output_offset_ms(const std::string& id, int offset_ms);

    void open_stream(const PcmFormat& format);
    uint64_t write_interleaved_pcm(const void* frames, size_t bytes);
    void flush();
    void stop();

    uint64_t current_frame() const;
    bool stream_open() const;

private:
    static uint64_t frame_count_from_bytes(const PcmFormat& format, size_t bytes);
    static void validate_format(const PcmFormat& format);

    Transport& transport_;
    SyncClock clock_;
    PcmFormat format_;
    bool stream_open_ = false;
};

}  // namespace multiroom
