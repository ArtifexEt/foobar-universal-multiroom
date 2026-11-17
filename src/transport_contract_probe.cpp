#include <cstdint>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <airplay_crypto.h>

#include "../components/foo_out_multiroom_bridge/core/multiroom_engine.h"
#include "../components/foo_out_multiroom_bridge/core/output_preferences.h"
#include "../components/foo_out_multiroom_bridge/core/output_registry.h"
#include "../components/foo_out_multiroom_bridge/core/speaker_groups.h"
#include "../components/foo_out_multiroom_bridge/core/volume_control.h"
#include "../transports/airplay/airplay_timing.h"
#include "../transports/airplay/airplay_transport.h"

struct TransportCapability {
    std::string name;
    bool required = false;
};

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }

    return true;
}

bool exercise_speaker_groups() {
    multiroom::SpeakerGroup group{
        " downstairs ",
        " Downstairs | Evening ",
        {"living-room-old", "kitchen", "kitchen", ""},
    };
    group = multiroom::normalize_speaker_group(std::move(group));

    bool ok = true;
    ok &= expect(group.id == "downstairs" && group.name == "Downstairs | Evening",
                 "speaker group identity and name should be normalized");
    ok &= expect(group.output_ids == std::vector<std::string>({"living-room-old", "kitchen"}),
                 "speaker group members should be non-empty and unique");

    const auto encoded = multiroom::serialize_speaker_groups({group});
    const auto decoded = multiroom::deserialize_speaker_groups(encoded);
    ok &= expect(decoded.size() == 1 && decoded.front().id == group.id &&
                 decoded.front().name == group.name && decoded.front().output_ids == group.output_ids,
                 "speaker groups should survive a versioned settings round trip");

    multiroom::OutputDevice living;
    living.id = "living-room";
    living.name = "Living Room";
    living.aliases = {"living-room-old"};
    living.selected = true;
    multiroom::OutputDevice kitchen;
    kitchen.id = "kitchen";
    kitchen.name = "Kitchen";
    kitchen.selected = true;
    multiroom::OutputDevice office;
    office.id = "office";
    office.name = "Office";

    auto outputs = std::vector<multiroom::OutputDevice>{living, kitchen, office};
    const auto resolved = multiroom::resolve_speaker_group_output_ids(group, outputs);
    ok &= expect(resolved == std::vector<std::string>({"living-room", "kitchen"}),
                 "speaker groups should resolve persisted discovery aliases to current output IDs");
    ok &= expect(multiroom::speaker_group_contains_persisted_output(group, "living-room", outputs) &&
                 !multiroom::speaker_group_contains_persisted_output(group, "old-offline-selection", outputs),
                 "group activation should select aliased members and clear unrelated persisted offline outputs");
    ok &= expect(multiroom::speaker_group_matches_persisted_selection(
                     group, {"living-room-old", "kitchen"}, {}) &&
                 !multiroom::speaker_group_matches_persisted_selection(
                     group, {"living-room-old"}, {}) &&
                 !multiroom::speaker_group_matches_persisted_selection(
                     group, {"living-room-old", "kitchen", "old-offline-selection"}, {}),
                 "a queued group should require its full persisted membership before discovery completes");
    auto alternate_group = group;
    alternate_group.id = "alternate";
    alternate_group.output_ids = {"living-room-old", "office-offline"};
    ok &= expect(!multiroom::speaker_group_matches_persisted_selection(
                     group, {"living-room-old", "office-offline"}, outputs) &&
                 multiroom::speaker_group_matches_persisted_selection(
                     alternate_group, {"living-room-old", "office-offline"}, outputs),
                 "persisted offline members should disambiguate groups with the same available subset");
    ok &= expect(multiroom::speaker_group_matches_selection(group, outputs),
                 "speaker group should match the exact selected output set");
    outputs.back().selected = true;
    ok &= expect(!multiroom::speaker_group_matches_selection(group, outputs),
                 "speaker group should not match when an extra output is selected");
    return ok;
}
void print_capabilities() {
    const std::vector<TransportCapability> capabilities = {
        {"list_outputs", true},
        {"refresh_discovery", true},
        {"set_enabled_outputs", true},
        {"set_output_volume", true},
        {"set_output_offset_ms", true},
        {"now_playing_metadata", true},
        {"now_playing_artwork", true},
        {"accept_pcm_stream", true},
        {"transport_clock_sync", true},
        {"group_presets", false},
        {"speaker_auth", false},
    };

    std::cout << "Universal Multiroom Audio Bridge transport contract\n";
    for (const auto& capability : capabilities) {
        std::cout << "- " << capability.name
                  << (capability.required ? " required" : " optional")
                  << '\n';
    }
}

multiroom::OutputDevice make_airplay_loopback_output(
    std::string id,
    std::string name,
    uint16_t port) {
    multiroom::OutputDevice output;
    output.id = std::move(id);
    output.name = std::move(name);
    output.type = multiroom::OutputType::AirPlay;
    output.supports_airplay2 = true;
    output.requires_encrypted_stream = true;
    output.volume = 50;
    output.format = "airplay2-loopback";
    output.supported_formats = {"airplay2"};
    output.endpoint_host = "127.0.0.1";
    output.endpoint_port = port;
    return output;
}

std::string make_airplay_event_message(const fxchain::airplay::bplist::Value& event) {
    const auto encoded = fxchain::airplay::bplist::encode(event);
    const std::string body(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    return "POST /command RTSP/1.0\r\nCSeq: 7\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string make_airplay_remote_command_message(
    int command,
    const std::string& command_id,
    const std::string& type = "sendMediaRemoteCommand") {
    using namespace fxchain::airplay::bplist;
    Dict params;
    params.emplace_back(
        "kMRMediaRemoteOptionCommandID",
        Value::str(command_id));

    Dict root;
    root.emplace_back("modernMediaRemoteCommand", Value::str(std::to_string(command)));
    root.emplace_back("type", Value::str(type));
    root.emplace_back("params", Value::object(std::move(params)));
    return make_airplay_event_message(Value::object(std::move(root)));
}

class SelectiveFailingControlClient final : public multiroom::airplay::AirPlayControlClient {
public:
    enum class Request { Open, Close, Volume, Metadata };

    explicit SelectiveFailingControlClient(std::set<std::string> failing_outputs)
        : failing_outputs_(std::move(failing_outputs)) {}

    multiroom::airplay::AirPlayPairingResult pair(
        const multiroom::OutputDevice& output,
        const std::string& pin) override {
        static_cast<void>(pin);
        multiroom::airplay::AirPlayPairingResult result;
        result.credentials.output_id = output.id;
        result.credentials.client_id = "selective-client";
        result.credentials.controller_seed.assign(32, 0x33);
        result.credentials.accessory_identifier = {'s', 'e', 'l', 'e', 'c', 't', 'i', 'v', 'e'};
        result.credentials.accessory_public_key.assign(32, 0x44);
        return result;
    }

    void set_remote_command_handler(
        multiroom::airplay::AirPlayRemoteCommandHandler handler) override {
        static_cast<void>(handler);
    }

    multiroom::airplay::AirPlayNegotiatedSession open(
        const multiroom::OutputDevice& output,
        const multiroom::PcmFormat& format) override {
        static_cast<void>(format);
        wait_if_blocked(Request::Open);
        ++open_count_;
        if (failing_outputs_.find(output.id) != failing_outputs_.end()) {
            throw std::runtime_error("simulated open failure");
        }

        multiroom::airplay::AirPlayNegotiatedSession session;
        session.rtsp_session_id = "selective-" + output.id;
        session.stream_uri = "rtsp://" + output.endpoint_host + "/" + output.id;
        session.server_name = "selective-loopback";
        session.supported_methods = {"OPTIONS", "SETUP", "RECORD", "SET_PARAMETER", "FLUSH", "TEARDOWN"};
        session.ports.local_data_port = 6100;
        session.ports.local_control_port = 6101;
        session.ports.local_timing_port = 6102;
        session.ports.server_data_port = output.endpoint_port;
        session.ports.server_control_port = static_cast<uint16_t>(output.endpoint_port + 1);
        session.ports.server_timing_port = static_cast<uint16_t>(output.endpoint_port + 2);
        return session;
    }

    void send_audio(
        const std::string& output_id,
        const std::string& rtsp_session_id,
        const multiroom::ScheduledPacket& packet,
        const void* frames,
        size_t bytes) override {
        static_cast<void>(rtsp_session_id);
        static_cast<void>(packet);
        if (frames == nullptr && bytes != 0) {
            throw std::invalid_argument("Loopback audio frame buffer cannot be null when bytes are present.");
        }
        audio_output_ids_.push_back(output_id);
    }

    void set_volume(const std::string&, const std::string&, int) override {
        wait_if_blocked(Request::Volume);
    }
    void set_metadata(
        const std::string& id,
        const std::string&,
        const multiroom::PlaybackMetadata& metadata) override {
        wait_if_blocked(Request::Metadata);
        metadata_titles_.emplace_back(id, metadata.title);
    }
    void clear_metadata(const std::string&, const std::string&) override {}
    void flush(const std::string&, const std::string&) override {}
    void reset_pending_open_cancel() override {}
    void cancel_pending_open() override {}
    void close(const std::string&, const std::string&) override {
        wait_if_blocked(Request::Close);
    }

    size_t open_count() const { return open_count_; }
    const std::vector<std::string>& audio_output_ids() const { return audio_output_ids_; }
    const auto& metadata_titles() const { return metadata_titles_; }

    void block_next_request(Request request) {
        std::lock_guard lock(request_mutex_);
        blocked_request_ = request;
        request_entered_ = false;
    }

    bool wait_for_blocked_request(std::chrono::milliseconds timeout) {
        std::unique_lock lock(request_mutex_);
        return request_changed_.wait_for(lock, timeout, [&] { return request_entered_; });
    }

    void release_blocked_request() {
        std::lock_guard lock(request_mutex_);
        blocked_request_.reset();
        request_changed_.notify_all();
    }

private:
    void wait_if_blocked(Request request) {
        std::unique_lock lock(request_mutex_);
        if (blocked_request_ != request) return;
        request_entered_ = true;
        request_changed_.notify_all();
        request_changed_.wait(lock, [&] { return blocked_request_ != request; });
    }

    std::set<std::string> failing_outputs_;
    size_t open_count_ = 0;
    std::vector<std::string> audio_output_ids_;
    std::vector<std::pair<std::string, std::string>> metadata_titles_;
    std::mutex request_mutex_;
    std::condition_variable request_changed_;
    std::optional<Request> blocked_request_;
    bool request_entered_ = false;
};

bool exercise_volume_does_not_block_audio() {
    using namespace std::chrono_literals;

    auto control_client = std::make_shared<SelectiveFailingControlClient>(std::set<std::string>{});
    multiroom::airplay::AirPlaySessionManager sessions(control_client);
    auto output = make_airplay_loopback_output("living-room", "Living Room", 7200);
    output.selected = true;
    sessions.prepare_outputs({output});
    sessions.open_for_outputs({output}, {44100, 2, 16});

    control_client->block_next_request(SelectiveFailingControlClient::Request::Volume);
    auto volume_update = std::async(std::launch::async, [&] {
        sessions.set_volume(output.id, 25);
    });
    bool ok = expect(
        control_client->wait_for_blocked_request(1s),
        "test volume request should enter the simulated RTSP wait");

    const std::vector<int16_t> silence(704);
    multiroom::ScheduledPacket packet;
    packet.output_id = output.id;
    packet.bytes = silence.size() * sizeof(int16_t);
    packet.group_sync_anchor_valid = true;
    packet.group_sync_start_ntp = 123456;
    auto audio_write = std::async(std::launch::async, [&] {
        sessions.enqueue(packet, silence.data(), packet.bytes);
    });
    ok &= expect(
        audio_write.wait_for(250ms) == std::future_status::ready,
        "an in-flight receiver volume request must not block PCM packet delivery");

    control_client->release_blocked_request();
    volume_update.get();
    audio_write.get();
    return ok;
}

bool exercise_metadata_does_not_block_audio() {
    using namespace std::chrono_literals;

    auto client = std::make_shared<SelectiveFailingControlClient>(std::set<std::string>{});
    multiroom::airplay::AirPlayTransport transport(client);
    multiroom::MultiroomEngine engine(transport);
    transport.add_discovered_output(make_airplay_loopback_output("living-room", "Living Room", 7400));
    transport.add_discovered_output(make_airplay_loopback_output("kitchen", "Kitchen", 7401));
    engine.select_outputs({"living-room"});
    engine.open_stream({44100, 2, 16});
    transport.connect_selected_outputs();

    multiroom::PlaybackMetadata first;
    first.title = "First track";
    client->block_next_request(SelectiveFailingControlClient::Request::Metadata);
    auto first_update = std::async(std::launch::async, [&] { transport.set_playback_metadata(first); });
    bool ok = expect(client->wait_for_blocked_request(1s), "test metadata must enter the simulated RTSP wait");

    const std::vector<int16_t> silence(704);
    auto audio_write = std::async(std::launch::async, [&] {
        engine.write_interleaved_pcm(silence.data(), silence.size() * sizeof(int16_t));
    });
    ok &= expect(audio_write.wait_for(1s) == std::future_status::ready,
                 "waiting for a metadata acknowledgement must leave PCM delivery running");

    multiroom::PlaybackMetadata latest;
    latest.title = "Latest track";
    auto stage_latest = std::async(std::launch::async, [&] { transport.stage_playback_metadata(latest); });
    ok &= expect(stage_latest.wait_for(1s) == std::future_status::ready,
                 "a newer track snapshot can be queued while the receiver acknowledges the previous one");

    client->release_blocked_request();
    first_update.get();
    audio_write.get();
    stage_latest.get();

    transport.publish_playback_metadata();
    engine.select_outputs({"living-room", "kitchen"});
    transport.connect_selected_outputs();
    transport.publish_playback_metadata();

    ok &= expect(client->metadata_titles() == std::vector<std::pair<std::string, std::string>>{
                     {"living-room", "First track"}, {"living-room", "Latest track"}, {"kitchen", "Latest track"}},
                 "joining should replay current metadata only to the newly opened receiver");
    return ok;
}

bool exercise_session_reconfiguration_does_not_block_audio() {
    using namespace std::chrono_literals;
    using Request = SelectiveFailingControlClient::Request;

    auto client = std::make_shared<SelectiveFailingControlClient>(std::set<std::string>{});
    multiroom::airplay::AirPlaySessionManager sessions(client);
    const multiroom::PcmFormat format{44100, 2, 16};
    auto living_room = make_airplay_loopback_output("living-room", "Living Room", 7500);
    auto kitchen = make_airplay_loopback_output("kitchen", "Kitchen", 7501);
    living_room.selected = true;
    kitchen.selected = true;
    sessions.open_for_outputs({living_room}, format);

    const std::vector<int16_t> silence(704);
    multiroom::ScheduledPacket packet;
    packet.output_id = living_room.id;
    packet.bytes = silence.size() * sizeof(int16_t);
    const auto verify_pcm_during_request = [&](Request request, const auto& operation) {
        client->block_next_request(request);
        auto control = std::async(std::launch::async, operation);
        bool ok = expect(client->wait_for_blocked_request(1s),
                         "the receiver should enter the simulated lifecycle request");
        auto audio = std::async(std::launch::async, [&] {
            sessions.enqueue(packet, silence.data(), packet.bytes);
        });
        ok &= expect(audio.wait_for(1s) == std::future_status::ready,
                     "a retained receiver should receive PCM while another receiver opens or closes");
        client->release_blocked_request();
        control.get();
        audio.get();
        return ok;
    };

    bool ok = verify_pcm_during_request(Request::Open, [&] {
        sessions.open_for_outputs({living_room, kitchen}, format);
    });
    ok &= expect(sessions.ready_output_ids().contains(kitchen.id),
                 "joining should publish the receiver only after setup finishes");
    ok &= verify_pcm_during_request(Request::Close, [&] {
        sessions.close_missing_outputs({living_room});
    });
    ok &= expect(sessions.ready_output_ids() == std::set<std::string>{living_room.id},
                 "closed receivers should no longer accept PCM");

    living_room.endpoint_port = 7502;
    sessions.prepare_outputs({living_room});
    sessions.open_for_outputs({living_room}, format);
    ok &= expect(client->open_count() == 3,
                 "preparing an existing receiver for a new endpoint must not mask its endpoint change");
    return ok;
}

bool exercise_airplay_remote_commands() {
    using multiroom::airplay::AirPlayRemoteCommand;
    const std::vector<std::pair<int, AirPlayRemoteCommand>> mappings = {
        {0, AirPlayRemoteCommand::Play},
        {1, AirPlayRemoteCommand::Pause},
        {2, AirPlayRemoteCommand::TogglePlayPause},
        {3, AirPlayRemoteCommand::Stop},
        {4, AirPlayRemoteCommand::NextTrack},
        {5, AirPlayRemoteCommand::PreviousTrack},
    };

    bool ok = true;
    const auto supported_body = multiroom::airplay::make_airplay_remote_supported_commands_body();
    const auto supported = fxchain::airplay::bplist::decode(
        fxchain::airplay::Bytes(supported_body.begin(), supported_body.end()));
    const auto* supported_params = supported ? supported->find("params") : nullptr;
    const auto* supported_commands = supported_params ?
        supported_params->find("mrSupportedCommandsFromSender") : nullptr;
    ok &= expect(supported &&
                 supported->find("type") &&
                 supported->find("type")->asStr() == "updateMRSupportedCommands" &&
                 supported_commands &&
                 supported_commands->type == fxchain::airplay::bplist::Value::Type::Arr &&
                 supported_commands->arr.size() == 6,
                 "AirPlay sender should advertise six supported receiver playback commands");
    ok &= expect(multiroom::airplay::airplay_remote_command_advertisement_accepted(200) &&
                 !multiroom::airplay::airplay_remote_command_advertisement_accepted(404),
                 "receiver rejection of remote commands should remain a non-audio capability result");

    for (const auto& [number, expected] : mappings) {
        const auto command_id = "remote-command-" + std::to_string(number);
        const auto event = multiroom::airplay::parse_airplay_remote_command_message(
            make_airplay_remote_command_message(number, command_id));
        ok &= expect(event.has_value() &&
                     event->command == expected &&
                     event->command_id == command_id,
                     "AirPlay binary-plist remote command should preserve its action and command ID");
    }

    ok &= expect(
        !multiroom::airplay::parse_airplay_remote_command_message(
            make_airplay_remote_command_message(3, "not-a-command", "updateInfo")),
        "AirPlay updateInfo events should not be mistaken for playback commands");
    ok &= expect(
        !multiroom::airplay::parse_airplay_remote_command_message(
            make_airplay_remote_command_message(99, "unknown-command")),
        "unsupported AirPlay remote commands should be ignored");

    auto control_client = std::make_shared<multiroom::airplay::AirPlayLoopbackControlClient>();
    multiroom::airplay::AirPlayTransport transport(control_client);
    std::string received_output;
    std::optional<multiroom::airplay::AirPlayRemoteCommandEvent> received_event;
    transport.set_remote_command_handler([&](const auto& output_id, const auto& event) {
        received_output = output_id;
        received_event = event;
    });
    control_client->emit_remote_command(
        "living-room",
        {AirPlayRemoteCommand::Stop, "loopback-stop"});
    ok &= expect(received_output == "living-room" &&
                 received_event &&
                 received_event->command == AirPlayRemoteCommand::Stop &&
                 received_event->command_id == "loopback-stop",
                 "AirPlay transport should forward receiver playback commands to its consumer");

    using namespace fxchain::airplay::bplist;
    const auto parse_volume = [](Value volume, bool nested) {
        Dict event{
            {"type", Value::str("sendMediaRemoteCommand")},
            {"value", Value::str("dvlc")},
        };
        if (nested) {
            event.emplace_back("params", Value::object({{"volume", std::move(volume)}}));
        } else {
            event.emplace_back("volume", std::move(volume));
        }
        return multiroom::airplay::parse_airplay_remote_command_message(
            make_airplay_event_message(Value::object(std::move(event))));
    };

    const auto device_volume = parse_volume(Value::real(0.416), true);
    const auto minimum = parse_volume(Value::integer(0), false);
    const auto maximum = parse_volume(Value::integer(1), true);
    ok &= expect(device_volume && device_volume->command == AirPlayRemoteCommand::VolumeChanged &&
                 device_volume->volume_percent == 42 &&
                 minimum && minimum->volume_percent == 0 &&
                 maximum && maximum->volume_percent == 100,
                 "AirPlay receiver volume events should accept numeric values in both plist locations");
    ok &= expect(!parse_volume(Value::real(-0.1), false) &&
                 !parse_volume(Value::real(1.01), false) &&
                 !parse_volume(Value::real((std::numeric_limits<double>::quiet_NaN)()), true) &&
                 !parse_volume(Value::str("0.4"), true) &&
                 !parse_volume(Value::boolean(true), false),
                 "Malformed and out-of-range AirPlay receiver volume should be ignored");
    if (device_volume) {
        control_client->emit_remote_command("kitchen", *device_volume);
        ok &= expect(received_output == "kitchen" && received_event &&
                     received_event->command == AirPlayRemoteCommand::VolumeChanged &&
                     received_event->volume_percent == 42,
                     "Receiver volume feedback should identify the speaker that sent it");
    }
    return ok;
}

bool exercise_partial_airplay_open_failure() {
    bool ok = true;

    auto partial_client = std::make_shared<SelectiveFailingControlClient>(std::set<std::string>{"kitchen"});
    multiroom::airplay::AirPlayTransport partial_transport(partial_client);
    multiroom::MultiroomEngine partial_engine(partial_transport);
    partial_engine.start_discovery();
    partial_transport.add_discovered_output(make_airplay_loopback_output("living-room", "Living Room", 7100));
    partial_transport.add_discovered_output(make_airplay_loopback_output("kitchen", "Kitchen", 7101));
    partial_engine.select_outputs({"living-room", "kitchen"});
    partial_engine.open_stream({48000, 2, 16});
    partial_transport.connect_selected_outputs();

    const std::vector<int16_t> silence(480);
    partial_engine.write_interleaved_pcm(silence.data(), silence.size() * sizeof(int16_t));
    const auto partial_sessions = partial_transport.sessions();
    const auto partial_packets = partial_transport.queued_packets();

    bool saw_ready_living_room = false;
    bool saw_failed_kitchen = false;
    for (const auto& session : partial_sessions) {
        if (session.output_id == "living-room") {
            saw_ready_living_room = session.phase == multiroom::airplay::AirPlaySessionPhase::Ready && session.open;
        }
        if (session.output_id == "kitchen") {
            saw_failed_kitchen = session.phase == multiroom::airplay::AirPlaySessionPhase::Failed && !session.last_error.empty();
        }
    }

    ok &= expect(partial_client->open_count() == 2, "partial open should attempt every selected output");
    ok &= expect(saw_ready_living_room, "partial open should keep successful selected sessions ready");
    ok &= expect(saw_failed_kitchen, "partial open should retain failed selected session diagnostics");
    ok &= expect(partial_packets.size() == 1, "partial open should queue audio only for ready outputs");
    ok &= expect(partial_client->audio_output_ids().size() == 1 &&
                 partial_client->audio_output_ids().front() == "living-room",
                 "partial open should send RTP only to ready sessions");

    auto failing_client = std::make_shared<SelectiveFailingControlClient>(std::set<std::string>{"living-room", "kitchen"});
    multiroom::airplay::AirPlayTransport failing_transport(failing_client);
    multiroom::MultiroomEngine failing_engine(failing_transport);
    failing_engine.start_discovery();
    failing_transport.add_discovered_output(make_airplay_loopback_output("living-room", "Living Room", 7200));
    failing_transport.add_discovered_output(make_airplay_loopback_output("kitchen", "Kitchen", 7201));
    failing_engine.select_outputs({"living-room", "kitchen"});

    bool threw_all_failed = false;
    try {
        failing_engine.open_stream({48000, 2, 16});
        failing_transport.connect_selected_outputs();
    } catch (const std::exception&) {
        threw_all_failed = true;
    }
    ok &= expect(threw_all_failed, "connecting should still fail when no selected output can be opened");

    return ok;
}

bool exercise_no_selected_outputs_sink() {
    bool ok = true;

    auto control_client = std::make_shared<multiroom::airplay::AirPlayLoopbackControlClient>();
    multiroom::airplay::AirPlayTransport transport(control_client);
    multiroom::MultiroomEngine engine(transport);
    engine.start_discovery();
    transport.add_discovered_output(make_airplay_loopback_output("living-room", "Living Room", 7300));
    transport.add_discovered_output(make_airplay_loopback_output("kitchen", "Kitchen", 7301));

    engine.select_outputs({});
    engine.open_stream({44100, 2, 16});

    const std::vector<int16_t> silence(882);
    const auto timestamp = engine.write_interleaved_pcm(silence.data(), silence.size() * sizeof(int16_t));

    ok &= expect(timestamp == 0, "no selected output sink should accept first write at stream frame 0");
    ok &= expect(engine.current_frame() == 441, "no selected output sink should advance the stream clock");
    ok &= expect(control_client->open_count() == 0, "no selected output sink should not open AirPlay sessions");
    ok &= expect(control_client->audio_packet_count() == 0, "no selected output sink should not send RTP packets");
    ok &= expect(transport.queued_packets().empty(), "no selected output sink should not queue packets");

    engine.select_outputs({"living-room"});
    transport.connect_selected_outputs();
    engine.write_interleaved_pcm(silence.data(), silence.size() * sizeof(int16_t));
    const auto packets_sent = control_client->audio_packet_count();
    transport.retain_routed_outputs({"kitchen"});
    transport.set_routed_outputs({"kitchen"});
    engine.write_interleaved_pcm(silence.data(), silence.size() * sizeof(int16_t));
    ok &= expect(packets_sent > 0 && control_client->audio_packet_count() == packets_sent,
                 "replacing the current route with an unready receiver must cut off PCM without interrupting the stream");
    transport.close_unselected_output_sessions({});
    ok &= expect(control_client->close_count() == 1,
                 "an unavailable replacement group should still close the previous receiver");

    engine.flush();
    ok &= expect(engine.current_frame() == 0, "no selected output sink flush should reset the stream clock");
    engine.stop();
    ok &= expect(!engine.stream_open(), "no selected output sink stop should close the engine stream");

    return ok;
}

bool exercise_airplay_group_timing() {
    using multiroom::airplay::airplay_presentation_ntp;
    using multiroom::airplay::current_ntp_timestamp;
    using multiroom::airplay::ntp_interval;

    constexpr auto anchor = uint64_t{10} << 32;
    bool ok = expect(
        airplay_presentation_ntp(anchor, 0, 44100, -500) == (uint64_t{19} << 31) &&
            airplay_presentation_ntp(anchor, 22050, 44100, -500) == anchor &&
            airplay_presentation_ntp(anchor, 48000, 48000, 250) == (uint64_t{45} << 30),
        "signed offsets should move presentation time from the first frame at either sample rate");
    ok &= expect(airplay_presentation_ntp(UINT64_MAX - (uint64_t{1} << 31) + 1, 22050, 44100, 0) == 0,
                 "NTP arithmetic should cross the wire timestamp's era boundary");

    auto client = std::make_shared<multiroom::airplay::AirPlayLoopbackControlClient>();
    multiroom::airplay::AirPlayTransport transport(client);
    multiroom::MultiroomEngine engine(transport);
    transport.add_discovered_output(make_airplay_loopback_output("living-room", "Living Room", 7400));
    transport.add_discovered_output(make_airplay_loopback_output("kitchen", "Kitchen", 7401));
    engine.open_stream({44100, 2, 16});

    const std::vector<int16_t> silence(704);
    const auto bytes = silence.size() * sizeof(int16_t);
    // A silent period before the first session must not acquire a stale anchor.
    transport.write_frames(silence.data(), bytes, 0);
    engine.select_outputs({"living-room"});
    transport.connect_selected_outputs();
    const auto before_send = current_ntp_timestamp();
    constexpr uint64_t start_frame = 30 * 44100;
    transport.write_frames(silence.data(), bytes, start_frame);
    const auto after_send = current_ntp_timestamp();
    const auto initial_packets = transport.queued_packets();
    if (!expect(initial_packets.size() == 1, "the first active receiver should get one timing packet")) {
        return false;
    }
    const auto& first = initial_packets.front();
    const auto first_presentation = airplay_presentation_ntp(first.group_sync_start_ntp, start_frame, 44100, 0);
    const auto minimum_lead = ntp_interval(250, 1000);
    ok &= expect(first_presentation - before_send - minimum_lead <= after_send - before_send,
                 "the first active receiver should establish the group anchor with network lead");

    engine.select_outputs({"living-room", "kitchen"});
    transport.connect_selected_outputs();
    engine.set_output_offset_ms("kitchen", -500);
    const auto next_frame = start_frame + 352;
    transport.write_frames(silence.data(), bytes, next_frame);
    bool joined_on_group_anchor = false;
    for (const auto& packet : transport.queued_packets()) {
        if (packet.output_id == "kitchen") {
            joined_on_group_anchor = packet.stream_timestamp == next_frame && packet.offset_ms == -500 &&
                packet.group_sync_start_ntp == first.group_sync_start_ntp;
        }
    }
    ok &= expect(joined_on_group_anchor, "a joining receiver should keep the running group anchor");

    engine.flush();
    engine.write_interleaved_pcm(silence.data(), bytes);
    const auto after_flush = transport.queued_packets();
    ok &= expect(after_flush.size() == 2 && after_flush[0].stream_timestamp == 0 &&
                 after_flush[0].group_sync_start_ntp == after_flush[1].group_sync_start_ntp &&
                 after_flush[0].group_sync_start_ntp != first.group_sync_start_ntp,
                 "flush should start a fresh anchor shared by both receivers");
    return ok;
}

bool exercise_cancelled_airplay_open() {
    bool ok = true;
    auto control_client = std::make_shared<multiroom::airplay::AirPlayLoopbackControlClient>();
    multiroom::airplay::AirPlayTransport transport(control_client);
    multiroom::MultiroomEngine engine(transport);
    engine.start_discovery();
    transport.add_discovered_output(make_airplay_loopback_output("living-room", "Living Room", 7350));
    engine.select_outputs({"living-room"});
    engine.open_stream({44100, 2, 16});

    transport.cancel_pending_open();
    bool cancelled = false;
    try {
        transport.connect_selected_outputs();
    } catch (const std::exception&) {
        cancelled = true;
    }
    ok &= expect(cancelled, "cancelled AirPlay setup should abort before opening a session");
    ok &= expect(control_client->open_count() == 0, "cancelled setup should not call the control-client open path");

    transport.reset_pending_open_cancel();
    transport.set_enabled_outputs({});
    transport.set_enabled_outputs({"living-room"});
    ok &= expect(transport.list_outputs().front().selected,
                 "resetting a stopped setup should allow speaker selection while playback is stopped");
    transport.connect_selected_outputs();
    ok &= expect(control_client->open_count() == 1, "resetting cancellation should allow the next AirPlay setup");
    engine.stop();
    return ok;
}

bool exercise_failed_reselection_does_not_drop_pcm() {
    bool ok = true;

    auto control_client = std::make_shared<SelectiveFailingControlClient>(std::set<std::string>{"kitchen"});
    multiroom::airplay::AirPlayTransport transport(control_client);
    multiroom::MultiroomEngine engine(transport);
    engine.start_discovery();
    transport.add_discovered_output(make_airplay_loopback_output("living-room", "Living Room", 7400));
    transport.add_discovered_output(make_airplay_loopback_output("kitchen", "Kitchen", 7401));

    engine.select_outputs({"living-room"});
    engine.open_stream({48000, 2, 16});
    transport.connect_selected_outputs();

    const std::vector<int16_t> silence(480);
    engine.write_interleaved_pcm(silence.data(), silence.size() * sizeof(int16_t));
    ok &= expect(control_client->audio_output_ids().size() == 1 &&
                 control_client->audio_output_ids().front() == "living-room",
                 "initial selected output should receive PCM");

    engine.select_outputs({"kitchen"});
    bool connect_failed = false;
    try {
        transport.connect_selected_outputs();
    } catch (const std::exception&) {
        connect_failed = true;
    }
    ok &= expect(connect_failed, "failed reselection should report that no selected output opened");

    bool write_failed = false;
    try {
        engine.write_interleaved_pcm(silence.data(), silence.size() * sizeof(int16_t));
    } catch (const std::exception&) {
        write_failed = true;
    }
    ok &= expect(write_failed, "failed reselection should not silently drop PCM writes");
    ok &= expect(control_client->audio_output_ids().size() == 1,
                 "failed reselection should not send PCM to a stale ready session");

    return ok;
}

bool exercise_output_registry_retain() {
    bool ok = true;
    multiroom::OutputRegistry registry;
    registry.upsert(make_airplay_loopback_output("legacy-alias", "Speaker", 7500));
    registry.upsert(make_airplay_loopback_output("airplay2-identity", "Speaker", 7500));

    registry.retain({"airplay2-identity"});
    const auto outputs = registry.list();
    ok &= expect(outputs.size() == 1 && outputs.front().id == "airplay2-identity",
                 "registry retain should remove superseded discovery aliases");

    auto control_client = std::make_shared<multiroom::airplay::AirPlayLoopbackControlClient>();
    multiroom::airplay::AirPlayTransport transport(control_client);
    transport.start_discovery();
    auto legacy = make_airplay_loopback_output("legacy-alias", "Speaker", 7501);
    legacy.visible_in_dropdown = false;
    transport.add_discovered_output(std::move(legacy));
    transport.set_enabled_outputs({"legacy-alias"});
    transport.set_output_volume("legacy-alias", 37);

    auto canonical = make_airplay_loopback_output("airplay2-identity", "Speaker", 7601);
    transport.add_discovered_output(std::move(canonical));
    const auto migrated = transport.list_outputs();
    ok &= expect(migrated.size() == 1 &&
                 migrated.front().id == "airplay2-identity" &&
                 migrated.front().selected &&
                 migrated.front().volume == 37 &&
                 !migrated.front().visible_in_dropdown,
                 "AirPlay identity promotion should preserve alias selection, volume, and UI visibility");

    transport.set_enabled_outputs({});
    transport.set_enabled_outputs({"legacy-alias"});
    transport.set_output_volume("legacy-alias", 41);
    const auto stale_alias_update = transport.list_outputs();
    ok &= expect(stale_alias_update.size() == 1 &&
                 stale_alias_update.front().id == "airplay2-identity" &&
                 stale_alias_update.front().selected &&
                 stale_alias_update.front().volume == 41,
                 "AirPlay controls should resolve a stale discovery alias after identity promotion");
    return ok;
}

bool exercise_saved_output_preferences() {
    using namespace multiroom;
    auto saved = parse_output_preferences(
        "airplay:hardware|1|30\r\n"
        "airplay:key|0|70|0|-28\n"
        "airplay:missing|1|55|1\n");
    OutputDevice speaker;
    speaker.id = "airplay:key";
    speaker.aliases = {"airplay:hardware"};
    speaker.supports_airplay2 = true;
    speaker.endpoint_host = "127.0.0.1";
    speaker.endpoint_port = 7000;
    std::vector<OutputDevice> discovered{speaker};

    bool ok = expect(restore_output_preferences(saved, discovered),
                     "restoring should consolidate the saved legacy alias");
    ok &= expect(saved.size() == 2 && !discovered.front().selected &&
                 discovered.front().volume == 70 && !discovered.front().visible_in_dropdown &&
                 discovered.front().offset_ms == -28,
                 "canonical preferences should win over conflicting legacy aliases");

    auto& selected_speaker = discovered.front();
    selected_speaker.selected = true;
    record_output_preferences(saved, selected_speaker);
    ok &= expect(has_undiscovered_selection(saved, discovered) &&
                 selected_playback_ids(discovered) == std::vector<std::string>{"airplay:key"},
                 "discovery should allow a partial group while its missing member stays saved");
    auto missing = selected_speaker;
    missing.id = "airplay:missing";
    missing.aliases.clear();
    missing.endpoint_port = 0;
    discovered.push_back(missing);
    restore_output_preferences(saved, discovered);
    ok &= expect(discovered.back().selected && has_undiscovered_selection(saved, discovered) &&
                 selected_playback_ids(discovered).size() == 1,
                 "an unavailable saved member should stay checked without entering the playback set");
    discovered.back().endpoint_port = 7001;
    ok &= expect(!has_undiscovered_selection(saved, discovered) && selected_playback_ids(discovered).size() == 2,
                 "discovery should complete after every saved receiver has an endpoint");

    OutputDevice renamed;
    renamed.id = "airplay:room|speaker._airplay._tcp.local";
    renamed.selected = true;
    renamed.volume = 43;
    renamed.offset_ms = 250;
    record_output_preferences(saved, renamed);
    auto reread = parse_output_preferences(serialize_output_preferences(saved));
    ok &= expect(reread.size() == saved.size() && reread.back().output_id == renamed.id &&
                 reread.back().selected && reread.back().volume == 43 && reread.back().offset_ms == 250,
                 "selection should survive a save and restart with a delimiter in the speaker ID");

    airplay::AirPlayTransport transport(airplay::make_airplay_loopback_control_client());
    speaker.name = "Living Room";
    speaker.txt_records["pk"] = "1111";
    transport.add_discovered_output(speaker);
    speaker.id = "airplay:other-key";
    speaker.txt_records["pk"] = "2222";
    transport.add_discovered_output(speaker);
    ok &= expect(transport.list_outputs().size() == 2,
                 "distinct advertised public keys should stay separate even on the same host and name");
    return ok;
}

}  // namespace

int main() {
    print_capabilities();

    try {
        auto control_client = std::make_shared<multiroom::airplay::AirPlayLoopbackControlClient>();
        multiroom::airplay::AirPlayTransport transport(control_client);
        multiroom::MultiroomEngine engine(transport);

        engine.start_discovery();

        transport.add_discovered_output({
            "living-room",
            "Living Room",
            multiroom::OutputType::AirPlay,
            false,
            false,
            false,
            false,
            true,
            false,
            false,
            45,
            0,
            0,
            "airplay2-loopback",
            {"airplay2"},
            "127.0.0.1",
            7000,
            {},
        });
        transport.add_discovered_output({
            "kitchen",
            "Kitchen",
            multiroom::OutputType::AirPlay,
            false,
            false,
            false,
            false,
            true,
            false,
            false,
            35,
            12,
            0,
            "airplay2-loopback",
            {"airplay2"},
            "127.0.0.1",
            7001,
            {},
        });

        transport.set_measured_latency_ms("living-room", 20);
        transport.set_measured_latency_ms("kitchen", 35);

        engine.select_outputs({"living-room", "kitchen"});
        engine.set_output_volume("living-room", 55);
        engine.set_output_offset_ms("kitchen", 12);

        multiroom::PlaybackMetadata metadata;
        metadata.title = "Current Track";
        metadata.artist = "Current Artist";
        metadata.album = "Current Album";
        metadata.album_artist = "Album Artist";
        metadata.composer = "Composer";
        metadata.genre = "Genre";
        metadata.track_number = 3;
        metadata.disc_number = 1;
        metadata.year = 2026;
        metadata.duration_ms = 245000;
        metadata.position_ms = 15000;
        metadata.artwork_mime = "image/png";
        metadata.artwork = {0x89, 0x50, 0x4e, 0x47};
        transport.set_playback_metadata(metadata);

        engine.open_stream({48000, 2, 16});
        transport.connect_selected_outputs();
        transport.publish_playback_metadata();
        engine.set_output_volume("kitchen", 40);

        const std::vector<int16_t> silence(480);
        const auto timestamp = engine.write_interleaved_pcm(
            silence.data(),
            silence.size() * sizeof(int16_t));
        const auto outputs = engine.list_outputs();
        const auto packets = transport.queued_packets();
        const auto sessions = transport.sessions();

        bool ok = true;
        ok &= expect(timestamp == 0, "first write should start at stream frame 0");
        ok &= expect(engine.current_frame() == 240, "480 stereo int16 samples should advance 240 frames");
        ok &= expect(outputs.size() == 2, "two outputs should be discovered");
        ok &= expect(packets.size() == 2, "one packet should be queued for each selected output");
        ok &= expect(
            packets.size() == 2 &&
            packets[0].group_sync_anchor_valid &&
            packets[1].group_sync_anchor_valid &&
            packets[0].group_sync_start_ntp == packets[1].group_sync_start_ntp,
            "all selected receivers should use one shared group RTP/NTP timing anchor");
        ok &= expect(control_client->audio_packet_count() == 2, "one RTP audio packet should be sent for each selected output");
        ok &= expect(control_client->volume_set_count() == 3, "initial and updated AirPlay session volumes should be sent over RTSP");
        ok &= expect(control_client->metadata_set_count() == 2,
                     "current metadata should be replayed to every newly opened AirPlay session");
        ok &= expect(control_client->last_metadata().title == "Current Track" &&
                     control_client->last_metadata().artwork == metadata.artwork,
                     "AirPlay sessions should receive the current title and artwork snapshot");
        ok &= expect(sessions.size() == 2, "two AirPlay sessions should exist");

        const auto dmap = multiroom::airplay::make_airplay_dmap_metadata_body(metadata);
        ok &= expect(dmap.size() > 8 && dmap.substr(0, 4) == "mlit",
                     "AirPlay metadata should use a DMAP listing-item container");
        ok &= expect(dmap.find("minm") != std::string::npos &&
                     dmap.find("Current Track") != std::string::npos &&
                     dmap.find("asar") != std::string::npos &&
                     dmap.find("Current Artist") != std::string::npos &&
                     dmap.find("asac") != std::string::npos,
                     "DMAP metadata should contain title, artist, and artwork state tags");
        ok &= expect(multiroom::airplay::airplay_progress_display_start(0) == 0,
                     "metadata progress should not underflow before the first RTP packet");
        ok &= expect(multiroom::airplay::airplay_progress_display_start(20000) == 4640,
                     "metadata progress should retain its receiver display lead after startup");
        ok &= expect(
            multiroom::effective_output_volume_percent(100, 100) == 100 &&
            multiroom::effective_output_volume_percent(50, 80) == 40 &&
            multiroom::effective_output_volume_percent(0, 100) == 0,
            "receiver attenuation should equal master volume multiplied by speaker volume");
        ok &= expect(
            multiroom::speaker_volume_from_receiver_percent(50, 40) == 80 &&
            multiroom::speaker_volume_from_receiver_percent(50, 65) == 100 &&
            !multiroom::speaker_volume_from_receiver_percent(0, 45),
            "receiver volume should map to the speaker ratio without dividing by a muted master");
        ok &= expect(std::abs(multiroom::airplay::airplay_volume_db(0) - (-144.0)) < 0.0001 &&
                     std::abs(multiroom::airplay::airplay_volume_db(50) - (-15.0)) < 0.0001 &&
                     std::abs(multiroom::airplay::airplay_volume_db(100)) < 0.0001,
                     "AirPlay volume should map foobar percentages to receiver -30..0 dB values");
        ok &= expect(
            multiroom::airplay::make_airplay_progress_parameter_body(metadata, 48000, 960000) ==
                "progress: 224640/960000/12000000\r\n",
            "AirPlay progress should include the complete 245 second track end timestamp");

        bool sessions_ready = true;
        for (const auto& session : sessions) {
            sessions_ready = sessions_ready &&
                session.phase == multiroom::airplay::AirPlaySessionPhase::Ready &&
                session.open &&
                session.negotiated_format.sample_rate == 48000 &&
                session.negotiated_format.channels == 2 &&
                session.negotiated_format.bits_per_sample == 16 &&
                !session.rtsp_session_id.empty() &&
                !session.stream_uri.empty() &&
                !session.rtsp_supported_methods.empty() &&
                session.transport_ports.local_data_port != 0 &&
                session.transport_ports.local_control_port != 0 &&
                session.transport_ports.local_timing_port != 0 &&
                session.transport_ports.server_data_port != 0 &&
                !session.endpoint_host.empty() &&
                session.endpoint_port != 0;
        }
        ok &= expect(sessions_ready, "selected AirPlay sessions should negotiate a ready PCM stream");

        transport.set_initial_output_volume("living-room", 30);
        engine.select_outputs({"living-room", "kitchen"});
        transport.connect_selected_outputs();
        ok &= expect(transport.queued_packets().size() == 2, "reselecting ready sessions should preserve queued packets");
        ok &= expect(control_client->volume_set_count() == 3,
                     "reselecting an active speaker should not reset the receiver's current volume");

        bool saw_kitchen_offset = false;
        for (const auto& packet : packets) {
            std::cout << "Packet " << packet.output_id
                      << " source=" << packet.stream_timestamp
                      << " presentation=" << packet.presentation_timestamp
                      << " earliest_send=" << packet.earliest_send_timestamp
                      << " lead=" << packet.lead_frames
                      << '\n';

            if (packet.output_id == "kitchen") {
                saw_kitchen_offset = packet.presentation_timestamp == 576;
            }
        }
        ok &= expect(saw_kitchen_offset, "12 ms kitchen offset at 48 kHz should be 576 frames");

        uint64_t last_packet_timestamp = 0;
        for (int index = 0; index < 300; ++index) {
            last_packet_timestamp = engine.write_interleaved_pcm(silence.data(), silence.size() * sizeof(int16_t));
        }
        for (const auto& session : transport.sessions()) {
            ok &= expect(session.queued_packets.size() == 256 &&
                         session.queued_packets.back().stream_timestamp == last_packet_timestamp,
                         "long playback should retain only the most recent diagnostic packets per speaker");
        }

        engine.flush();
        ok &= expect(transport.queued_packets().empty(), "flush should clear queued packets");
        ok &= expect(engine.current_frame() == 0, "flush should reset stream clock");
        ok &= expect(control_client->flush_count() == 2, "flush should notify each active AirPlay control session");

        engine.stop();
        ok &= expect(control_client->metadata_clear_count() == 2,
                     "stopping playback should clear stale metadata on every active AirPlay session");
        ok &= expect(!engine.stream_open(), "stop should close engine stream");
        ok &= expect(!transport.stream_open(), "stop should close transport stream");
        ok &= expect(control_client->close_count() == 2, "stop should close each AirPlay control session");
        ok &= exercise_partial_airplay_open_failure();
        ok &= exercise_no_selected_outputs_sink();
        ok &= exercise_airplay_group_timing();
        ok &= exercise_cancelled_airplay_open();
        ok &= exercise_failed_reselection_does_not_drop_pcm();
        ok &= exercise_output_registry_retain();
        ok &= exercise_airplay_remote_commands();
        ok &= exercise_volume_does_not_block_audio();
        ok &= exercise_metadata_does_not_block_audio();
        ok &= exercise_session_reconfiguration_does_not_block_audio();
        ok &= exercise_speaker_groups();
        ok &= exercise_saved_output_preferences();

        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "FAILED with exception: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
