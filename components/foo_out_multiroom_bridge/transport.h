#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace multiroom {

enum class OutputType {
    Unknown,
    AirPlay,
    Chromecast,
    Local,
    Heos,
    Fifo,
};

struct OutputDevice {
    std::string id;
    std::string name;
    OutputType type = OutputType::Unknown;
    bool selected = false;
    bool has_password = false;
    bool requires_auth = false;
    bool needs_auth_key = false;
    int volume = 0;
    int offset_ms = 0;
    int measured_latency_ms = 0;
    std::string format;
    std::vector<std::string> supported_formats;
    std::string endpoint_host;
    uint16_t endpoint_port = 0;
    std::map<std::string, std::string> txt_records;
};

struct PcmFormat {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint32_t bits_per_sample = 16;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual void start_discovery() = 0;
    virtual void stop_discovery() = 0;
    virtual std::vector<OutputDevice> list_outputs() = 0;
    virtual void set_enabled_outputs(const std::vector<std::string>& ids) = 0;
    virtual void set_output_volume(const std::string& id, int volume) = 0;
    virtual void set_output_offset_ms(const std::string& id, int offset_ms) = 0;
    virtual void open_stream(const PcmFormat& format) = 0;
    virtual void write_frames(const void* frames, size_t bytes, uint64_t stream_timestamp) = 0;
    virtual void flush() = 0;
    virtual void stop() = 0;
};

}  // namespace multiroom
