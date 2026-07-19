#pragma once

#include "core/packet_scheduler.h"
#include "transport.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace multiroom::airplay {

struct AirPlayRtspResponse {
    int status_code = 0;
    std::string reason;
    std::map<std::string, std::string> headers;
    std::string body;

    bool successful() const;
    std::string header(const std::string& name) const;
};

struct AirPlayTransportPorts {
    uint16_t local_data_port = 0;
    uint16_t local_control_port = 0;
    uint16_t local_timing_port = 0;
    uint16_t server_data_port = 0;
    uint16_t server_control_port = 0;
    uint16_t server_timing_port = 0;
};

struct AirPlayNegotiatedSession {
    std::string rtsp_session_id;
    std::string stream_uri;
    std::string server_name;
    std::vector<std::string> supported_methods;
    AirPlayTransportPorts ports;
};

struct AirPlayPairingCredentials {
    std::string output_id;
    std::string client_id;
    std::vector<uint8_t> controller_seed;
    std::vector<uint8_t> accessory_identifier;
    std::vector<uint8_t> accessory_public_key;

    bool valid() const;
};

struct AirPlayPairingResult {
    AirPlayPairingCredentials credentials;
    bool stored = false;
};

enum class AirPlayRemoteCommand {
    Play,
    Pause,
    TogglePlayPause,
    Stop,
    NextTrack,
    PreviousTrack,
};

struct AirPlayRemoteCommandEvent {
    AirPlayRemoteCommand command = AirPlayRemoteCommand::TogglePlayPause;
    std::string command_id;
};

using AirPlayRemoteCommandHandler = std::function<void(
    const std::string& output_id,
    const AirPlayRemoteCommandEvent& event)>;

class AirPlayPairingStore {
public:
    virtual ~AirPlayPairingStore() = default;
    virtual std::optional<AirPlayPairingCredentials> load(const std::string& output_id) = 0;
    virtual void save(const AirPlayPairingCredentials& credentials) = 0;
};

class AirPlayControlClient {
public:
    virtual ~AirPlayControlClient() = default;

    virtual AirPlayPairingResult pair(const OutputDevice& output, const std::string& pin) = 0;
    virtual void set_remote_command_handler(AirPlayRemoteCommandHandler handler) = 0;
    virtual AirPlayNegotiatedSession open(const OutputDevice& output, const PcmFormat& format) = 0;
    virtual void send_audio(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        const ScheduledPacket& packet,
        const void* frames,
        size_t bytes) = 0;
    virtual void set_volume(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        int volume) = 0;
    virtual void set_metadata(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        const PlaybackMetadata& metadata) = 0;
    virtual void clear_metadata(
        const std::string& output_id,
        const std::string& rtsp_session_id) = 0;
    virtual void flush(const std::string& output_id, const std::string& rtsp_session_id) = 0;
    virtual void close(const std::string& output_id, const std::string& rtsp_session_id) = 0;
};

class AirPlayRtspControlClient final : public AirPlayControlClient {
public:
    explicit AirPlayRtspControlClient(std::shared_ptr<AirPlayPairingStore> pairing_store = {});
    ~AirPlayRtspControlClient() override;

    AirPlayPairingResult pair(const OutputDevice& output, const std::string& pin) override;
    void set_remote_command_handler(AirPlayRemoteCommandHandler handler) override;
    AirPlayNegotiatedSession open(const OutputDevice& output, const PcmFormat& format) override;
    void send_audio(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        const ScheduledPacket& packet,
        const void* frames,
        size_t bytes) override;
    void set_volume(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        int volume) override;
    void set_metadata(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        const PlaybackMetadata& metadata) override;
    void clear_metadata(
        const std::string& output_id,
        const std::string& rtsp_session_id) override;
    void flush(const std::string& output_id, const std::string& rtsp_session_id) override;
    void close(const std::string& output_id, const std::string& rtsp_session_id) override;

private:
    class Connection;

    std::optional<AirPlayPairingCredentials> load_pairing_credentials(const OutputDevice& output);
    void save_pairing_credentials(const AirPlayPairingCredentials& credentials);
    void dispatch_remote_command(
        const std::string& output_id,
        const AirPlayRemoteCommandEvent& event);

    std::shared_ptr<AirPlayPairingStore> pairing_store_;
    std::map<std::string, AirPlayPairingCredentials> memory_pairing_credentials_;
    std::mutex remote_command_mutex_;
    AirPlayRemoteCommandHandler remote_command_handler_;
    std::map<std::string, std::unique_ptr<Connection>> connections_;
};

class AirPlayLoopbackControlClient final : public AirPlayControlClient {
public:
    AirPlayPairingResult pair(const OutputDevice& output, const std::string& pin) override;
    void set_remote_command_handler(AirPlayRemoteCommandHandler handler) override;
    AirPlayNegotiatedSession open(const OutputDevice& output, const PcmFormat& format) override;
    void send_audio(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        const ScheduledPacket& packet,
        const void* frames,
        size_t bytes) override;
    void set_volume(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        int volume) override;
    void set_metadata(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        const PlaybackMetadata& metadata) override;
    void clear_metadata(
        const std::string& output_id,
        const std::string& rtsp_session_id) override;
    void flush(const std::string& output_id, const std::string& rtsp_session_id) override;
    void close(const std::string& output_id, const std::string& rtsp_session_id) override;

    size_t open_count() const;
    size_t audio_packet_count() const;
    size_t volume_set_count() const;
    size_t metadata_set_count() const;
    size_t metadata_clear_count() const;
    PlaybackMetadata last_metadata() const;
    size_t flush_count() const;
    size_t close_count() const;
    void emit_remote_command(
        const std::string& output_id,
        const AirPlayRemoteCommandEvent& event);

private:
    size_t open_count_ = 0;
    size_t audio_packet_count_ = 0;
    size_t volume_set_count_ = 0;
    size_t metadata_set_count_ = 0;
    size_t metadata_clear_count_ = 0;
    PlaybackMetadata last_metadata_;
    size_t flush_count_ = 0;
    size_t close_count_ = 0;
    std::mutex remote_command_mutex_;
    AirPlayRemoteCommandHandler remote_command_handler_;
};

std::shared_ptr<AirPlayControlClient> make_airplay_rtsp_control_client(std::shared_ptr<AirPlayPairingStore> pairing_store = {});
std::shared_ptr<AirPlayControlClient> make_airplay_loopback_control_client();

std::string make_airplay_dmap_metadata_body(const PlaybackMetadata& metadata);
std::string make_airplay_remote_supported_commands_body();
bool airplay_remote_command_advertisement_accepted(int status_code);
uint32_t airplay_progress_display_start(uint32_t track_start);
std::optional<AirPlayRemoteCommandEvent> parse_airplay_remote_command_message(
    const std::string& message);

}  // namespace multiroom::airplay
