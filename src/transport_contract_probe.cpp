#include <iostream>
#include <string>
#include <vector>

#include "../components/foo_out_multiroom_bridge/core/sync_clock.h"
#include "../transports/airplay/airplay_transport.h"

struct TransportCapability {
    std::string name;
    bool required = false;
};

int main() {
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

    multiroom::airplay::AirPlayTransport transport;
    transport.start_discovery();

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

    transport.set_enabled_outputs({"living-room", "kitchen"});
    transport.open_stream({48000, 2, 16});

    multiroom::SyncClock clock(48000);
    const std::vector<int16_t> silence(480);
    const auto timestamp = clock.advance(static_cast<uint64_t>(silence.size() / 2));
    transport.write_frames(silence.data(), silence.size() * sizeof(int16_t), timestamp);

    std::cout << "Discovered outputs: " << transport.list_outputs().size() << '\n';
    std::cout << "Queued packets: " << transport.queued_packets().size() << '\n';
    for (const auto& packet : transport.queued_packets()) {
        std::cout << "Packet " << packet.output_id
                  << " source=" << packet.stream_timestamp
                  << " presentation=" << packet.presentation_timestamp
                  << " earliest_send=" << packet.earliest_send_timestamp
                  << '\n';
    }
    std::cout << "Next stream frame: " << clock.current_frame() << '\n';

    return 0;
}
