#pragma once

#include "../../components/foo_out_multiroom_bridge/core/output_registry.h"
#include "../../components/foo_out_multiroom_bridge/core/sync_clock.h"
#include "../../components/foo_out_multiroom_bridge/transport.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace multiroom::airplay {

struct QueuedPacket {
    std::string output_id;
    uint64_t stream_timestamp = 0;
    size_t bytes = 0;
};

class AirPlayTransport final : public Transport {
public:
    void start_discovery() override;
    void stop_discovery() override;
    std::vector<OutputDevice> list_outputs() override;
    void set_enabled_outputs(const std::vector<std::string>& ids) override;
    void set_output_volume(const std::string& id, int volume) override;
    void set_output_offset_ms(const std::string& id, int offset_ms) override;
    void open_stream(const PcmFormat& format) override;
    void write_frames(const void* frames, size_t bytes, uint64_t stream_timestamp) override;
    void flush() override;
    void stop() override;

    void add_discovered_output(OutputDevice device);
    bool discovery_active() const;
    bool stream_open() const;
    const std::vector<QueuedPacket>& queued_packets() const;

private:
    OutputRegistry registry_;
    PcmFormat stream_format_;
    bool discovery_active_ = false;
    bool stream_open_ = false;
    std::vector<QueuedPacket> queued_packets_;
};

}  // namespace multiroom::airplay

