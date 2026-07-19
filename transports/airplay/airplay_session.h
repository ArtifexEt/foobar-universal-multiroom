#pragma once

#include "airplay_rtsp.h"
#include "core/packet_scheduler.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace multiroom::airplay {

enum class AirPlaySessionPhase {
    Closed,
    Connecting,
    Ready,
    Failed,
};

struct AirPlaySessionState {
    std::string output_id;
    std::string output_name;
    std::string endpoint_host;
    uint16_t endpoint_port = 0;
    AirPlaySessionPhase phase = AirPlaySessionPhase::Closed;
    bool open = false;
    PcmFormat negotiated_format;
    std::string rtsp_session_id;
    std::string stream_uri;
    std::string rtsp_server;
    std::vector<std::string> rtsp_supported_methods;
    AirPlayTransportPorts transport_ports;
    std::string last_error;
    std::vector<ScheduledPacket> queued_packets;
};

class AirPlaySessionManager {
public:
    explicit AirPlaySessionManager(std::shared_ptr<AirPlayControlClient> control_client = make_airplay_rtsp_control_client());

    AirPlayPairingResult pair_output(const OutputDevice& output, const std::string& pin);
    void prepare_outputs(const std::vector<OutputDevice>& outputs);
    void open_for_outputs(const std::vector<OutputDevice>& outputs, const PcmFormat& format);
    void close_missing_outputs(const std::vector<OutputDevice>& outputs);
    void set_volume(const std::string& output_id, int volume);
    void set_metadata(const PlaybackMetadata& metadata);
    void clear_metadata();
    void enqueue(const ScheduledPacket& packet, const void* frames, size_t bytes);
    void flush();
    void stop();

    std::vector<AirPlaySessionState> sessions() const;
    std::vector<ScheduledPacket> queued_packets() const;
    std::set<std::string> ready_output_ids() const;

private:
    void close_control_session(AirPlaySessionState& session);

    mutable std::mutex mutex_;
    std::shared_ptr<AirPlayControlClient> control_client_;
    std::map<std::string, AirPlaySessionState> sessions_;
};

}  // namespace multiroom::airplay
