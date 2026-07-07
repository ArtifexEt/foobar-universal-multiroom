#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
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
        multiroom::airplay::AirPlayTransport transport;
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
            45,
            0,
            0,
            "pcm",
            {"pcm"},
        });
        transport.add_discovered_output({
            "kitchen",
            "Kitchen",
            multiroom::OutputType::AirPlay,
            false,
            false,
            false,
            false,
            35,
            12,
            0,
            "pcm",
            {"pcm"},
        });

        transport.set_measured_latency_ms("living-room", 20);
        transport.set_measured_latency_ms("kitchen", 35);

        engine.select_outputs({"living-room", "kitchen"});
        engine.set_output_volume("living-room", 55);
        engine.set_output_offset_ms("kitchen", 12);
        engine.open_stream({48000, 2, 16});

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
        ok &= expect(sessions.size() == 2, "two AirPlay sessions should exist");

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

        engine.stop();
        ok &= expect(!engine.stream_open(), "stop should close engine stream");
        ok &= expect(!transport.stream_open(), "stop should close transport stream");

        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "FAILED with exception: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
