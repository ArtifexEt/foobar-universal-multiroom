#pragma once

#include "airplay_discovery.h"
#include "airplay_session.h"

#include "core/output_registry.h"
#include "core/packet_scheduler.h"
#include "transport.h"

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace multiroom::airplay {

class AirPlayTransport final : public Transport {
public:
    explicit AirPlayTransport(std::shared_ptr<AirPlayControlClient> control_client = make_airplay_rtsp_control_client());

    void start_discovery() override;
    void stop_discovery() override;
    void refresh_discovery(std::chrono::milliseconds timeout = std::chrono::milliseconds(1500)) override;
    std::vector<OutputDevice> list_outputs() override;
    AirPlayPairingResult pair_output(const std::string& id, const std::string& pin);
    void set_enabled_outputs(const std::vector<std::string>& ids) override;
    void set_output_volume(const std::string& id, int volume) override;
    void set_output_offset_ms(const std::string& id, int offset_ms) override;
    void set_playback_metadata(const PlaybackMetadata& metadata) override;
    void clear_playback_metadata() override;
    void open_stream(const PcmFormat& format) override;
    void connect_selected_outputs();
    void write_frames(const void* frames, size_t bytes, uint64_t stream_timestamp) override;
    void flush() override;
    void stop() override;

    void add_discovered_output(OutputDevice device);
    void set_measured_latency_ms(const std::string& id, int measured_latency_ms);
    bool discovery_active() const;
    bool stream_open() const;
    std::vector<ScheduledPacket> queued_packets() const;
    std::vector<AirPlaySessionState> sessions() const;

private:
    void sync_discovered_outputs();
    std::string resolve_output_id(const std::string& id) const;
    std::vector<std::string> resolve_output_ids(const std::vector<std::string>& ids) const;

    AirPlayDiscovery discovery_;
    OutputRegistry registry_;
    AirPlaySessionManager sessions_;
    PacketScheduler scheduler_;
    PcmFormat stream_format_;
    bool discovery_active_ = false;
    bool stream_open_ = false;
    std::optional<PlaybackMetadata> playback_metadata_;
};

}  // namespace multiroom::airplay
