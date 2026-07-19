#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <airplay_crypto.h>

#include "../components/foo_out_multiroom_bridge/core/multiroom_engine.h"
#include "../components/foo_out_multiroom_bridge/core/output_registry.h"
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
    const auto encoded = encode(Value::object(std::move(root)));
    const std::string body(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    return "POST /command RTSP/1.0\r\nCSeq: 7\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
}

class SelectiveFailingControlClient final : public multiroom::airplay::AirPlayControlClient {
public:
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

    void set_volume(const std::string&, const std::string&, int) override {}
    void set_metadata(
        const std::string&,
        const std::string&,
        const multiroom::PlaybackMetadata&) override {}
    void clear_metadata(const std::string&, const std::string&) override {}
    void flush(const std::string&, const std::string&) override {}
    void reset_pending_open_cancel() override {}
    void cancel_pending_open() override {}
    void close(const std::string&, const std::string&) override {}

    size_t open_count() const { return open_count_; }
    const std::vector<std::string>& audio_output_ids() const { return audio_output_ids_; }

private:
    std::set<std::string> failing_outputs_;
    size_t open_count_ = 0;
    std::vector<std::string> audio_output_ids_;
};

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

    engine.select_outputs({});
    engine.open_stream({44100, 2, 16});

    const std::vector<int16_t> silence(882);
    const auto timestamp = engine.write_interleaved_pcm(silence.data(), silence.size() * sizeof(int16_t));

    ok &= expect(timestamp == 0, "no selected output sink should accept first write at stream frame 0");
    ok &= expect(engine.current_frame() == 441, "no selected output sink should advance the stream clock");
    ok &= expect(control_client->open_count() == 0, "no selected output sink should not open AirPlay sessions");
    ok &= expect(control_client->audio_packet_count() == 0, "no selected output sink should not send RTP packets");
    ok &= expect(transport.queued_packets().empty(), "no selected output sink should not queue packets");

    engine.flush();
    ok &= expect(engine.current_frame() == 0, "no selected output sink flush should reset the stream clock");
    engine.stop();
    ok &= expect(!engine.stream_open(), "no selected output sink stop should close the engine stream");

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

        engine.select_outputs({"living-room", "kitchen"});
        ok &= expect(transport.queued_packets().size() == 2, "reselecting ready sessions should preserve queued packets");

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
        ok &= exercise_cancelled_airplay_open();
        ok &= exercise_failed_reselection_does_not_drop_pcm();
        ok &= exercise_output_registry_retain();
        ok &= exercise_airplay_remote_commands();

        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "FAILED with exception: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
