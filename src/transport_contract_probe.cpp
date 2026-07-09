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

#include "../components/foo_out_multiroom_bridge/core/multiroom_engine.h"
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
        {"accept_pcm_stream", true},
        {"transport_clock_sync", true},
        {"group_presets", false},
        {"speaker_auth", false},
    };

    std::cout << "Foobar Universal Multiroom transport contract\n";
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
    void flush(const std::string&, const std::string&) override {}
    void close(const std::string&, const std::string&) override {}

    size_t open_count() const { return open_count_; }
    const std::vector<std::string>& audio_output_ids() const { return audio_output_ids_; }

private:
    std::set<std::string> failing_outputs_;
    size_t open_count_ = 0;
    std::vector<std::string> audio_output_ids_;
};

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
        ok &= expect(sessions.size() == 2, "two AirPlay sessions should exist");

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
        ok &= expect(!engine.stream_open(), "stop should close engine stream");
        ok &= expect(!transport.stream_open(), "stop should close transport stream");
        ok &= expect(control_client->close_count() == 2, "stop should close each AirPlay control session");
        ok &= exercise_partial_airplay_open_failure();
        ok &= exercise_no_selected_outputs_sink();
        ok &= exercise_failed_reselection_does_not_drop_pcm();

        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "FAILED with exception: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
