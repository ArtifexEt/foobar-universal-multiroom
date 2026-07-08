#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../transports/airplay/airplay_transport.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

struct ProbeOptions {
    std::chrono::milliseconds timeout{2500};
    std::chrono::milliseconds duration{5000};
    std::string target = "first";
    bool play = false;
    bool require_speaker = false;
    bool list_txt = false;
    int volume = 35;
};

void print_usage() {
    std::cout
        << "Usage:\n"
        << "  MultiroomAirPlayNetworkProbe [--timeout-ms n] [--list-txt]\n"
        << "  MultiroomAirPlayNetworkProbe --play [--target first|all|id] [--duration-ms n] [--volume 0-100] [--require-speaker]\n";
}

int parse_int_arg(const std::string& text, const char* name) {
    try {
        size_t parsed = 0;
        const int value = std::stoi(text, &parsed, 10);
        if (parsed != text.size()) {
            throw std::invalid_argument("trailing text");
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + text);
    }
}

ProbeOptions parse_options(int argc, char** argv) {
    ProbeOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--timeout-ms" && i + 1 < argc) {
            options.timeout = std::chrono::milliseconds(parse_int_arg(argv[++i], "--timeout-ms"));
        } else if (arg == "--duration-ms" && i + 1 < argc) {
            options.duration = std::chrono::milliseconds(parse_int_arg(argv[++i], "--duration-ms"));
        } else if (arg == "--target" && i + 1 < argc) {
            options.target = argv[++i];
        } else if (arg == "--volume" && i + 1 < argc) {
            options.volume = std::clamp(parse_int_arg(argv[++i], "--volume"), 0, 100);
        } else if (arg == "--play") {
            options.play = true;
        } else if (arg == "--require-speaker") {
            options.require_speaker = true;
        } else if (arg == "--list-txt") {
            options.list_txt = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (options.timeout.count() <= 0) {
        throw std::runtime_error("--timeout-ms must be greater than zero.");
    }
    if (options.duration.count() <= 0) {
        throw std::runtime_error("--duration-ms must be greater than zero.");
    }
    return options;
}

const char* yes_no(bool value) {
    return value ? "yes" : "no";
}

void print_outputs(const std::vector<multiroom::OutputDevice>& outputs, bool list_txt) {
    std::cout << "Discovered " << outputs.size() << " AirPlay device(s).\n";
    for (const auto& output : outputs) {
        std::cout
            << "- " << (output.name.empty() ? output.id : output.name)
            << "\n  id: " << output.id
            << "\n  endpoint: " << output.endpoint_host << ":" << output.endpoint_port
            << "\n  format: " << output.format
            << "\n  airplay2: " << yes_no(output.supports_airplay2)
            << ", legacy-l16: " << yes_no(output.supports_legacy_l16)
            << ", auth: " << yes_no(output.requires_auth)
            << ", encrypted: " << yes_no(output.requires_encrypted_stream)
            << "\n";

        if (list_txt && !output.txt_records.empty()) {
            std::cout << "  txt:\n";
            for (const auto& [key, value] : output.txt_records) {
                std::cout << "    " << key << "=" << value << "\n";
            }
        }
    }
}

std::vector<std::string> select_targets(
    const std::vector<multiroom::OutputDevice>& outputs,
    const std::string& target) {
    std::vector<std::string> ids;
    for (const auto& output : outputs) {
        if (!output.supports_airplay2) {
            continue;
        }
        if (target == "all" ||
            (target == "first" && ids.empty()) ||
            output.id == target ||
            output.name == target) {
            ids.push_back(output.id);
        }
    }
    return ids;
}

std::vector<int16_t> make_tone_chunk(
    uint32_t sample_rate,
    uint32_t channels,
    uint64_t frame_offset,
    uint32_t frames,
    double frequency_hz,
    double amplitude) {
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * channels);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const double phase = (static_cast<double>(frame_offset + frame) * frequency_hz * 2.0 * kPi) /
                             static_cast<double>(sample_rate);
        const auto sample = static_cast<int16_t>(std::clamp(
            std::sin(phase) * amplitude,
            -1.0,
            1.0) * static_cast<double>(std::numeric_limits<int16_t>::max()));
        for (uint32_t channel = 0; channel < channels; ++channel) {
            pcm[static_cast<size_t>(frame) * channels + channel] = sample;
        }
    }
    return pcm;
}

void print_session_summary(const std::vector<multiroom::airplay::AirPlaySessionState>& sessions) {
    for (const auto& session : sessions) {
        std::cout << "Session " << (session.output_name.empty() ? session.output_id : session.output_name)
                  << ": ";
        switch (session.phase) {
        case multiroom::airplay::AirPlaySessionPhase::Closed:
            std::cout << "closed";
            break;
        case multiroom::airplay::AirPlaySessionPhase::Connecting:
            std::cout << "connecting";
            break;
        case multiroom::airplay::AirPlaySessionPhase::Ready:
            std::cout << "ready";
            break;
        case multiroom::airplay::AirPlaySessionPhase::Failed:
            std::cout << "failed";
            break;
        }
        std::cout << ", endpoint=" << session.endpoint_host << ":" << session.endpoint_port;
        if (!session.last_error.empty()) {
            std::cout << ", error=" << session.last_error;
        }
        std::cout << "\n";
    }
}

bool play_probe_tone(
    multiroom::airplay::AirPlayTransport& transport,
    const std::vector<std::string>& target_ids,
    const ProbeOptions& options) {
    transport.set_enabled_outputs(target_ids);
    for (const auto& id : target_ids) {
        transport.set_output_volume(id, options.volume);
    }

    const multiroom::PcmFormat format{44100, 2, 16};
    transport.open_stream(format);

    constexpr uint32_t frames_per_chunk = 352;
    const auto end = std::chrono::steady_clock::now() + options.duration;
    uint64_t frame_offset = 0;
    size_t chunks = 0;
    while (std::chrono::steady_clock::now() < end) {
        auto pcm = make_tone_chunk(format.sample_rate, format.channels, frame_offset, frames_per_chunk, 440.0, 0.10);
        transport.write_frames(
            pcm.data(),
            pcm.size() * sizeof(int16_t),
            frame_offset);
        frame_offset += frames_per_chunk;
        ++chunks;
        std::this_thread::sleep_for(std::chrono::milliseconds(
            (frames_per_chunk * 1000) / format.sample_rate));
    }

    transport.flush();
    print_session_summary(transport.sessions());
    std::cout << "Sent " << chunks << " PCM chunk(s) to selected ready session(s).\n";
    transport.stop();
    return !transport.queued_packets().empty() || chunks > 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);

        multiroom::airplay::AirPlayTransport transport;
        transport.start_discovery();
        transport.refresh_discovery(options.timeout);
        auto outputs = transport.list_outputs();
        print_outputs(outputs, options.list_txt);

        if (outputs.empty()) {
            if (options.require_speaker) {
                std::cerr << "No AirPlay speakers were discovered.\n";
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }

        if (!options.play) {
            return EXIT_SUCCESS;
        }

        const auto targets = select_targets(outputs, options.target);
        if (targets.empty()) {
            std::cerr << "No AirPlay 2 target matched '" << options.target << "'.\n";
            print_session_summary(transport.sessions());
            return EXIT_FAILURE;
        }

        std::cout << "Selected " << targets.size() << " target(s) for playback probe.\n";
        const bool played = play_probe_tone(transport, targets, options);
        return played ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
