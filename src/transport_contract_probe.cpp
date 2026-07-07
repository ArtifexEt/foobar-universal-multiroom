#include <iostream>
#include <string>
#include <vector>

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

    return 0;
}

