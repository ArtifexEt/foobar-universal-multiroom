#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
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
        ok &= expect(control_client->volume_set_count() == 1, "active AirPlay session volume should be sent over RTSP");
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

        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "FAILED with exception: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
