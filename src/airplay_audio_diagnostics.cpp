#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../transports/airplay/airplay_transport.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kFramesPerChunk = 352;

struct Options {
    std::chrono::milliseconds timeout{4000};
    std::chrono::milliseconds duration{5000};
    std::string target = "korytarz";
    int volume = 45;
    double frequency = 880.0;
    std::string pin;
    bool require_speaker = false;
    bool list_only = false;
    bool loopback_self_test = false;
};

void print_usage() {
    std::cout
        << "Usage:\n"
        << "  MultiroomAirPlayAudioDiagnostics [--target name|id|first] [--timeout-ms n]\n"
        << "                                      [--duration-ms n] [--volume 0-100]\n"
        << "                                      [--frequency hz] [--pin nnnn] [--require-speaker]\n"
        << "  MultiroomAirPlayAudioDiagnostics --list-only [--timeout-ms n]\n"
        << "  MultiroomAirPlayAudioDiagnostics --loopback-self-test\n";
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

double parse_double_arg(const std::string& text, const char* name) {
    try {
        size_t parsed = 0;
        const double value = std::stod(text, &parsed);
        if (parsed != text.size()) {
            throw std::invalid_argument("trailing text");
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + text);
    }
}

Options parse_options(int argc, char** argv) {
    Options options;
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
        } else if (arg == "--frequency" && i + 1 < argc) {
            options.frequency = parse_double_arg(argv[++i], "--frequency");
        } else if (arg == "--pin" && i + 1 < argc) {
            options.pin = argv[++i];
        } else if (arg == "--require-speaker") {
            options.require_speaker = true;
        } else if (arg == "--list-only") {
            options.list_only = true;
        } else if (arg == "--loopback-self-test") {
            options.loopback_self_test = true;
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
    if (options.frequency <= 0.0) {
        throw std::runtime_error("--frequency must be greater than zero.");
    }
    return options;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool target_matches(const multiroom::OutputDevice& output, const std::string& target) {
    if (target == "first") {
        return true;
    }

    const auto wanted = lower_ascii(target);
    const auto id = lower_ascii(output.id);
    const auto name = lower_ascii(output.name);
    return id == wanted ||
           name == wanted ||
           (!wanted.empty() && id.find(wanted) != std::string::npos) ||
           (!wanted.empty() && name.find(wanted) != std::string::npos);
}

const char* yes_no(bool value) {
    return value ? "yes" : "no";
}

void print_outputs(const std::vector<multiroom::OutputDevice>& outputs) {
    std::cout << "DISCOVERY devices=" << outputs.size() << "\n";
    for (const auto& output : outputs) {
        std::cout
            << "DEVICE name=\"" << (output.name.empty() ? output.id : output.name) << "\""
            << " id=\"" << output.id << "\""
            << " endpoint=" << output.endpoint_host << ":" << output.endpoint_port
            << " airplay2=" << yes_no(output.supports_airplay2)
            << " auth=" << yes_no(output.requires_auth)
            << " encrypted=" << yes_no(output.requires_encrypted_stream)
            << " legacy_l16=" << yes_no(output.supports_legacy_l16)
            << " format=\"" << output.format << "\"\n";
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
        if (target_matches(output, target)) {
            ids.push_back(output.id);
            if (target == "first") {
                break;
            }
        }
    }
    return ids;
}

std::vector<int16_t> make_tone_chunk(
    uint32_t sample_rate,
    uint32_t channels,
    uint64_t frame_offset,
    double frequency_hz,
    double amplitude) {
    std::vector<int16_t> pcm(static_cast<size_t>(kFramesPerChunk) * channels);
    for (uint32_t frame = 0; frame < kFramesPerChunk; ++frame) {
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

const char* phase_name(multiroom::airplay::AirPlaySessionPhase phase) {
    switch (phase) {
    case multiroom::airplay::AirPlaySessionPhase::Closed:
        return "closed";
    case multiroom::airplay::AirPlaySessionPhase::Connecting:
        return "connecting";
    case multiroom::airplay::AirPlaySessionPhase::Ready:
        return "ready";
    case multiroom::airplay::AirPlaySessionPhase::Failed:
        return "failed";
    }
    return "unknown";
}

void print_sessions(const multiroom::airplay::AirPlayTransport& transport, const char* stage) {
    const auto sessions = transport.sessions();
    std::cout << "SESSIONS stage=" << stage << " count=" << sessions.size() << "\n";
    for (const auto& session : sessions) {
        std::cout
            << "SESSION name=\"" << (session.output_name.empty() ? session.output_id : session.output_name) << "\""
            << " id=\"" << session.output_id << "\""
            << " phase=" << phase_name(session.phase)
            << " open=" << yes_no(session.open)
            << " endpoint=" << session.endpoint_host << ":" << session.endpoint_port
            << " data_port=" << session.transport_ports.server_data_port;
        if (!session.last_error.empty()) {
            std::cout << " error=\"" << session.last_error << "\"";
        }
        std::cout << "\n";
    }
}

multiroom::OutputDevice make_loopback_output(std::string id, std::string name, uint16_t port) {
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

bool run_audio_diagnostic(multiroom::airplay::AirPlayTransport& transport, const Options& options) {
    if (options.loopback_self_test) {
        transport.add_discovered_output(make_loopback_output("korytarz", "Korytarz", 7510));
    } else {
        transport.start_discovery();
        transport.refresh_discovery(options.timeout);
    }

    const auto outputs = transport.list_outputs();
    print_outputs(outputs);
    if (outputs.empty()) {
        if (options.require_speaker) {
            std::cerr << "FAIL no AirPlay speakers discovered.\n";
            return false;
        }
        return true;
    }
    if (options.list_only) {
        return true;
    }

    const auto target_ids = select_targets(outputs, options.target);
    if (target_ids.empty()) {
        std::cerr << "FAIL no AirPlay 2 target matched \"" << options.target << "\".\n";
        return false;
    }

    std::cout << "TARGETS count=" << target_ids.size();
    for (const auto& id : target_ids) {
        std::cout << " \"" << id << "\"";
    }
    std::cout << "\n";

    if (!options.pin.empty()) {
        if (target_ids.size() != 1) {
            std::cerr << "FAIL --pin requires exactly one selected target.\n";
            return false;
        }
        try {
            auto result = transport.pair_output(target_ids.front(), options.pin);
            std::cout << "PAIR id=\"" << target_ids.front() << "\" stored=" << yes_no(result.stored) << "\n";
        } catch (const std::exception& e) {
            std::cerr << "FAIL pair: " << e.what() << "\n";
            return false;
        }
    } else {
        for (const auto& output : outputs) {
            if (std::find(target_ids.begin(), target_ids.end(), output.id) != target_ids.end() &&
                output.requires_auth) {
                std::cout << "AUTH target=\"" << (output.name.empty() ? output.id : output.name)
                          << "\" advertises AirPlay auth; trying transient pairing first.\n";
            }
        }
    }

    transport.set_enabled_outputs(target_ids);
    for (const auto& id : target_ids) {
        transport.set_output_volume(id, options.volume);
    }

    const multiroom::PcmFormat format{44100, 2, 16};
    transport.open_stream(format);
    try {
        transport.connect_selected_outputs();
    } catch (const std::exception& e) {
        print_sessions(transport, "connect-failed");
        std::cerr << "FAIL connect: " << e.what() << "\n";
        return false;
    }
    print_sessions(transport, "connected");

    const auto end = std::chrono::steady_clock::now() + options.duration;
    uint64_t frame_offset = 0;
    size_t chunks = 0;
    size_t queued_before_flush = 0;
    try {
        while (std::chrono::steady_clock::now() < end) {
            auto pcm = make_tone_chunk(format.sample_rate, format.channels, frame_offset, options.frequency, 0.12);
            transport.write_frames(pcm.data(), pcm.size() * sizeof(int16_t), frame_offset);
            frame_offset += kFramesPerChunk;
            ++chunks;
            std::this_thread::sleep_for(std::chrono::milliseconds(
                (kFramesPerChunk * 1000) / format.sample_rate));
        }
        queued_before_flush = transport.queued_packets().size();
        transport.flush();
    } catch (const std::exception& e) {
        print_sessions(transport, "write-failed");
        std::cerr << "FAIL write: " << e.what() << "\n";
        transport.stop();
        return false;
    }

    print_sessions(transport, "after-tone");
    std::cout << "AUDIO chunks=" << chunks << " queued_packets_before_flush=" << queued_before_flush
              << " duration_ms=" << options.duration.count()
              << " frequency_hz=" << options.frequency
              << " volume=" << options.volume << "\n";
    transport.stop();
    return chunks > 0 && queued_before_flush > 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        auto control_client = options.loopback_self_test
            ? multiroom::airplay::make_airplay_loopback_control_client()
            : multiroom::airplay::make_airplay_rtsp_control_client();
        multiroom::airplay::AirPlayTransport transport(control_client);
        return run_audio_diagnostic(transport, options) ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
