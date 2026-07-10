#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dns_sd.h>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../components/foo_out_multiroom_bridge/core/multiroom_engine.h"
#include "../transports/airplay/airplay_transport.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kFramesPerChunk = 352;
constexpr const char* kAirPlayService = "_airplay._tcp.local";
constexpr const char* kRaopService = "_raop._tcp.local";

struct Options {
    std::chrono::milliseconds timeout{4000};
    std::chrono::milliseconds duration{5000};
    std::string target = "korytarz";
    int volume = 45;
    double frequency = 880.0;
    std::string pin;
    bool require_speaker = false;
    bool list_only = false;
};

struct BrowseEntry {
    std::string service_name;
    std::string regtype;
    std::string domain;
    uint32_t interface_index = 0;
};

struct DiscoveredService {
    std::string service_type;
    std::string full_name;
    std::string instance_name;
    std::string target_host;
    uint16_t port = 0;
    std::map<std::string, std::string> txt;
};

void print_usage(const char* program_name) {
    const std::string program = program_name == nullptr || program_name[0] == '\0'
        ? "MultiroomMacPlaybackTester"
        : program_name;
    std::cout
        << "Usage:\n"
        << "  " << program << " [--target name|id|first] [--timeout-ms n]\n"
        << "                            [--duration-ms n] [--volume 0-100]\n"
        << "                            [--frequency hz] [--pin nnnn] [--require-speaker]\n"
        << "  " << program << " --list-only [--timeout-ms n]\n";
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
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argc > 0 ? argv[0] : nullptr);
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

std::string trim_trailing_dot(std::string value) {
    while (!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    return value;
}

std::string decode_dns_sd_escapes(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size();) {
        if (value[index] == '\\' &&
            index + 3 < value.size() &&
            std::isdigit(static_cast<unsigned char>(value[index + 1])) &&
            std::isdigit(static_cast<unsigned char>(value[index + 2])) &&
            std::isdigit(static_cast<unsigned char>(value[index + 3]))) {
            const int decoded = (value[index + 1] - '0') * 100 +
                                (value[index + 2] - '0') * 10 +
                                (value[index + 3] - '0');
            result.push_back(static_cast<char>(decoded));
            index += 4;
            continue;
        }
        if (value[index] == '\\' && index + 1 < value.size()) {
            result.push_back(value[index + 1]);
            index += 2;
            continue;
        }

        result.push_back(value[index++]);
    }
    return result;
}

std::string join_regtype_domain(std::string regtype, std::string domain) {
    regtype = trim_trailing_dot(std::move(regtype));
    domain = trim_trailing_dot(std::move(domain));
    if (domain.empty()) {
        return regtype;
    }
    return regtype + "." + domain;
}

bool ends_with_service(const std::string& name, const char* service) {
    const auto lowered_name = lower_ascii(trim_trailing_dot(name));
    const auto lowered_service = lower_ascii(service);
    if (lowered_name.size() <= lowered_service.size() + 1) {
        return false;
    }
    return lowered_name.compare(
               lowered_name.size() - lowered_service.size(),
               lowered_service.size(),
               lowered_service) == 0 &&
           lowered_name[lowered_name.size() - lowered_service.size() - 1] == '.';
}

std::string infer_service_type(const std::string& full_name) {
    if (ends_with_service(full_name, kAirPlayService)) {
        return kAirPlayService;
    }
    if (ends_with_service(full_name, kRaopService)) {
        return kRaopService;
    }
    return {};
}

std::string service_instance_name(const std::string& full_name, const std::string& service_type) {
    const auto normalized_name = trim_trailing_dot(full_name);
    if (normalized_name.size() > service_type.size() + 1 &&
        lower_ascii(normalized_name).compare(
            normalized_name.size() - service_type.size(),
            service_type.size(),
            lower_ascii(service_type)) == 0) {
        auto instance = normalized_name.substr(0, normalized_name.size() - service_type.size() - 1);
        const auto at = instance.find('@');
        if (at != std::string::npos && at + 1 < instance.size()) {
            instance.erase(0, at + 1);
        }
        return instance;
    }
    return normalized_name;
}

std::string service_instance_prefix(const std::string& full_name, const std::string& service_type) {
    const auto normalized_name = trim_trailing_dot(full_name);
    if (!service_type.empty() &&
        normalized_name.size() > service_type.size() + 1 &&
        lower_ascii(normalized_name).compare(
            normalized_name.size() - service_type.size(),
            service_type.size(),
            lower_ascii(service_type)) == 0) {
        return normalized_name.substr(0, normalized_name.size() - service_type.size() - 1);
    }
    return normalized_name;
}

std::string normalize_hardware_id(std::string value) {
    value = lower_ascii(std::move(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return c == ':' || c == '-' || std::isspace(c) != 0;
    }), value.end());
    return value;
}

std::string txt_value(const std::map<std::string, std::string>& txt, const char* key) {
    const auto it = txt.find(key);
    return it == txt.end() ? std::string{} : it->second;
}

bool truthy_txt(const std::map<std::string, std::string>& txt, const char* key) {
    const auto value = lower_ascii(txt_value(txt, key));
    return value == "1" || value == "true" || value == "yes";
}

bool csv_number_contains(const std::string& text, unsigned expected) {
    size_t cursor = 0;
    while (cursor <= text.size()) {
        const auto comma = text.find(',', cursor);
        auto token = text.substr(cursor, comma == std::string::npos ? std::string::npos : comma - cursor);
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }), token.end());

        if (!token.empty()) {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(token.c_str(), &end, 0);
            if (end != token.c_str() && *end == '\0' && parsed == expected) {
                return true;
            }
        }

        if (comma == std::string::npos) {
            break;
        }
        cursor = comma + 1;
    }
    return false;
}

bool supports_unencrypted_audio(const std::map<std::string, std::string>& txt) {
    const auto encryption_types = txt_value(txt, "et");
    return encryption_types.empty() || csv_number_contains(encryption_types, 0);
}

bool txt_number_missing_or_contains(const std::map<std::string, std::string>& txt, const char* key, unsigned expected) {
    const auto value = txt_value(txt, key);
    return value.empty() || csv_number_contains(value, expected);
}

bool supports_mvp_l16_audio(const std::map<std::string, std::string>& txt) {
    return txt_number_missing_or_contains(txt, "cn", 0) &&
           txt_number_missing_or_contains(txt, "ss", 16) &&
           txt_number_missing_or_contains(txt, "ch", 2) &&
           txt_number_missing_or_contains(txt, "sr", 44100);
}

bool supports_airplay2_audio(const std::map<std::string, std::string>& txt) {
    const auto encryption_types = txt_value(txt, "et");
    const bool has_airplay2_encryption = csv_number_contains(encryption_types, 4) ||
                                         csv_number_contains(encryption_types, 5);
    const bool has_airplay_features = !txt_value(txt, "features").empty() ||
                                      !txt_value(txt, "ft").empty() ||
                                      !txt_value(txt, "vv").empty() ||
                                      !txt_value(txt, "protovers").empty();
    return has_airplay2_encryption || has_airplay_features;
}

void normalize_service(DiscoveredService& service) {
    service.full_name = trim_trailing_dot(service.full_name);
    service.service_type = trim_trailing_dot(service.service_type);
    service.target_host = trim_trailing_dot(service.target_host);

    if (service.service_type.empty()) {
        service.service_type = infer_service_type(service.full_name);
    }
    if (service.instance_name.empty() && !service.service_type.empty()) {
        service.instance_name = decode_dns_sd_escapes(service_instance_name(service.full_name, service.service_type));
    }
}

std::string device_id_from(const DiscoveredService& service) {
    auto id = txt_value(service.txt, "pk");
    if (id.empty()) {
        id = txt_value(service.txt, "deviceid");
        if (!id.empty()) {
            id = normalize_hardware_id(std::move(id));
        }
    }
    if (id.empty()) {
        auto instance = service_instance_prefix(service.full_name, service.service_type);
        const auto at = instance.find('@');
        if (at != std::string::npos && at > 0) {
            id = normalize_hardware_id(instance.substr(0, at));
        }
    }
    if (id.empty()) {
        id = service.full_name;
    }
    return "airplay:" + lower_ascii(id);
}

multiroom::OutputDevice to_output_device(const DiscoveredService& service) {
    auto normalized_service = service;
    normalize_service(normalized_service);

    multiroom::OutputDevice device;
    device.id = device_id_from(normalized_service);
    device.name = normalized_service.instance_name.empty() ? normalized_service.full_name : normalized_service.instance_name;
    device.type = multiroom::OutputType::AirPlay;
    device.has_password = truthy_txt(normalized_service.txt, "pw");
    const bool unencrypted_audio = supports_unencrypted_audio(normalized_service.txt);
    device.supports_legacy_l16 = unencrypted_audio && supports_mvp_l16_audio(normalized_service.txt);
    device.supports_airplay2 = supports_airplay2_audio(normalized_service.txt);
    device.requires_encrypted_stream = !unencrypted_audio || device.supports_airplay2;
    device.requires_auth = device.has_password ||
                           truthy_txt(normalized_service.txt, "da") ||
                           truthy_txt(normalized_service.txt, "requiresauth") ||
                           device.requires_encrypted_stream;
    device.needs_auth_key = device.requires_encrypted_stream;
    device.volume = 50;
    if (device.supports_airplay2) {
        device.format = "airplay2";
        device.supported_formats = {"airplay2"};
    } else if (device.supports_legacy_l16) {
        device.format = "airplay-legacy-l16";
        device.supported_formats = {"airplay-legacy-l16"};
    } else {
        device.format = device.requires_auth ? "airplay-auth-required" : "airplay-unsupported-format";
        device.supported_formats = {device.format};
    }
    device.endpoint_host = normalized_service.target_host;
    device.endpoint_port = normalized_service.port;
    device.txt_records = normalized_service.txt;
    return device;
}

bool has_endpoint(const multiroom::OutputDevice& device) {
    return !device.endpoint_host.empty() && device.endpoint_port != 0;
}

void merge_device(multiroom::OutputDevice& target, const multiroom::OutputDevice& candidate) {
    const bool prefer_candidate_endpoint = candidate.supports_airplay2 && has_endpoint(candidate);

    if (target.name.empty()) {
        target.name = candidate.name;
    }
    if ((prefer_candidate_endpoint || target.endpoint_host.empty()) && !candidate.endpoint_host.empty()) {
        target.endpoint_host = candidate.endpoint_host;
    }
    if ((prefer_candidate_endpoint || target.endpoint_port == 0) && candidate.endpoint_port != 0) {
        target.endpoint_port = candidate.endpoint_port;
    }

    target.has_password = target.has_password || candidate.has_password;
    target.requires_auth = target.requires_auth || candidate.requires_auth;
    target.needs_auth_key = target.needs_auth_key || candidate.needs_auth_key;
    target.supports_airplay2 = target.supports_airplay2 || candidate.supports_airplay2;
    target.supports_legacy_l16 = target.supports_legacy_l16 || candidate.supports_legacy_l16;
    target.requires_encrypted_stream = target.requires_encrypted_stream || candidate.requires_encrypted_stream;

    for (const auto& format : candidate.supported_formats) {
        if (std::find(target.supported_formats.begin(), target.supported_formats.end(), format) ==
            target.supported_formats.end()) {
            target.supported_formats.push_back(format);
        }
    }
    for (const auto& [key, value] : candidate.txt_records) {
        target.txt_records.try_emplace(key, value);
    }

    if (target.supports_airplay2) {
        target.format = "airplay2";
    } else if (target.supports_legacy_l16) {
        target.format = "airplay-legacy-l16";
    } else if (target.format.empty()) {
        target.format = candidate.format;
    }
}

void append_or_merge_device(std::vector<multiroom::OutputDevice>& devices, multiroom::OutputDevice candidate) {
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return device.id == candidate.id;
    });
    if (it == devices.end()) {
        devices.push_back(std::move(candidate));
        return;
    }

    merge_device(*it, candidate);
}

class DnsServiceRef final {
public:
    DnsServiceRef() = default;
    explicit DnsServiceRef(DNSServiceRef ref) : ref_(ref) {}
    ~DnsServiceRef() {
        if (ref_ != nullptr) {
            DNSServiceRefDeallocate(ref_);
        }
    }

    DnsServiceRef(const DnsServiceRef&) = delete;
    DnsServiceRef& operator=(const DnsServiceRef&) = delete;

    DNSServiceRef get() const {
        return ref_;
    }

private:
    DNSServiceRef ref_ = nullptr;
};

bool process_dns_service_until(
    DNSServiceRef ref,
    std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>& done) {
    const int fd = DNSServiceRefSockFD(ref);
    if (fd < 0) {
        return false;
    }

    while (!done() && std::chrono::steady_clock::now() < deadline) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        const auto remaining_us = std::chrono::duration_cast<std::chrono::microseconds>(remaining);
        timeval timeout = {};
        timeout.tv_sec = static_cast<time_t>(remaining_us.count() / 1000000);
        timeout.tv_usec = static_cast<suseconds_t>(remaining_us.count() % 1000000);

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);

        const int selected = select(fd + 1, &read_set, nullptr, nullptr, &timeout);
        if (selected < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (selected == 0) {
            continue;
        }
        if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) {
            return false;
        }
    }

    return done();
}

struct BrowseContext {
    std::vector<BrowseEntry> entries;
    std::set<std::string> seen;
};

void DNSSD_API browse_callback(
    DNSServiceRef,
    DNSServiceFlags flags,
    uint32_t interface_index,
    DNSServiceErrorType error_code,
    const char* service_name,
    const char* regtype,
    const char* reply_domain,
    void* context) {
    if (error_code != kDNSServiceErr_NoError || (flags & kDNSServiceFlagsAdd) == 0 || context == nullptr) {
        return;
    }

    auto* browse = static_cast<BrowseContext*>(context);
    BrowseEntry entry;
    entry.service_name = service_name == nullptr ? std::string{} : service_name;
    entry.regtype = regtype == nullptr ? std::string{} : regtype;
    entry.domain = reply_domain == nullptr ? std::string{} : reply_domain;
    entry.interface_index = interface_index;

    const auto key = std::to_string(entry.interface_index) + "|" + entry.service_name + "|" +
                     entry.regtype + "|" + entry.domain;
    if (!entry.service_name.empty() && browse->seen.insert(key).second) {
        browse->entries.push_back(std::move(entry));
    }
}

std::vector<BrowseEntry> browse_bonjour_service(const char* regtype, std::chrono::milliseconds timeout) {
    BrowseContext context;
    DNSServiceRef raw_ref = nullptr;
    const auto error = DNSServiceBrowse(
        &raw_ref,
        0,
        0,
        regtype,
        "local.",
        browse_callback,
        &context);
    if (error != kDNSServiceErr_NoError) {
        return {};
    }

    DnsServiceRef ref(raw_ref);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    static_cast<void>(process_dns_service_until(ref.get(), deadline, [] { return false; }));
    return context.entries;
}

struct ResolveContext {
    BrowseEntry entry;
    std::optional<DiscoveredService> service;
    bool complete = false;
};

void DNSSD_API resolve_callback(
    DNSServiceRef,
    DNSServiceFlags,
    uint32_t,
    DNSServiceErrorType error_code,
    const char* full_name,
    const char* host_target,
    uint16_t port,
    uint16_t txt_len,
    const unsigned char* txt_record,
    void* context) {
    if (context == nullptr) {
        return;
    }

    auto* resolve = static_cast<ResolveContext*>(context);
    resolve->complete = true;
    if (error_code != kDNSServiceErr_NoError) {
        return;
    }

    DiscoveredService service;
    service.full_name = full_name == nullptr ? std::string{} : full_name;
    service.service_type = join_regtype_domain(resolve->entry.regtype, resolve->entry.domain);
    service.instance_name = decode_dns_sd_escapes(service_instance_name(service.full_name, service.service_type));
    service.target_host = host_target == nullptr ? std::string{} : host_target;
    service.port = ntohs(port);

    const uint16_t item_count = TXTRecordGetCount(txt_len, txt_record);
    for (uint16_t index = 0; index < item_count; ++index) {
        char key[256] = {};
        uint8_t value_len = 0;
        const void* value = nullptr;
        const auto item_error = TXTRecordGetItemAtIndex(
            txt_len,
            txt_record,
            index,
            sizeof(key),
            key,
            &value_len,
            &value);
        if (item_error != kDNSServiceErr_NoError || key[0] == '\0') {
            continue;
        }
        const auto lowered_key = lower_ascii(key);
        const auto* bytes = static_cast<const char*>(value);
        service.txt[lowered_key] = bytes == nullptr ? std::string{} : std::string(bytes, bytes + value_len);
    }

    normalize_service(service);
    resolve->service = std::move(service);
}

std::optional<DiscoveredService> resolve_bonjour_entry(
    const BrowseEntry& entry,
    std::chrono::milliseconds timeout) {
    ResolveContext context;
    context.entry = entry;

    DNSServiceRef raw_ref = nullptr;
    const auto error = DNSServiceResolve(
        &raw_ref,
        0,
        entry.interface_index,
        entry.service_name.c_str(),
        entry.regtype.c_str(),
        entry.domain.c_str(),
        resolve_callback,
        &context);
    if (error != kDNSServiceErr_NoError) {
        return std::nullopt;
    }

    DnsServiceRef ref(raw_ref);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    static_cast<void>(process_dns_service_until(ref.get(), deadline, [&] { return context.complete; }));
    return context.service;
}

std::vector<multiroom::OutputDevice> discover_outputs(std::chrono::milliseconds timeout) {
    std::vector<multiroom::OutputDevice> devices;

    for (const auto* service_type : {"_airplay._tcp", "_raop._tcp"}) {
        const auto entries = browse_bonjour_service(service_type, timeout);
        for (const auto& entry : entries) {
            auto resolved = resolve_bonjour_entry(entry, timeout);
            if (!resolved) {
                continue;
            }
            append_or_merge_device(devices, to_output_device(*resolved));
        }
    }

    return devices;
}

const char* yes_no(bool value) {
    return value ? "yes" : "no";
}

void print_outputs(const std::vector<multiroom::OutputDevice>& outputs, bool include_txt) {
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
        if (include_txt) {
            for (const auto& [key, value] : output.txt_records) {
                std::cout << "TXT id=\"" << output.id << "\" " << key << "=" << value << "\n";
            }
        }
    }
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

bool run_audio_diagnostic(multiroom::airplay::AirPlayTransport& transport, const Options& options) {
    const auto discovered_outputs = discover_outputs(options.timeout);
    for (auto output : discovered_outputs) {
        transport.add_discovered_output(std::move(output));
    }

    const auto outputs = transport.list_outputs();
    print_outputs(outputs, options.list_only);
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
    }

    multiroom::MultiroomEngine engine(transport);

    engine.select_outputs(target_ids);
    for (const auto& id : target_ids) {
        engine.set_output_volume(id, options.volume);
    }

    const multiroom::PcmFormat format{44100, 2, 16};
    engine.open_stream(format);
    try {
        transport.connect_selected_outputs();
    } catch (const std::exception& e) {
        print_sessions(transport, "connect-failed");
        std::cerr << "FAIL connect: " << e.what() << "\n";
        engine.stop();
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
            engine.write_interleaved_pcm(pcm.data(), pcm.size() * sizeof(int16_t));
            frame_offset += kFramesPerChunk;
            ++chunks;
            std::this_thread::sleep_for(std::chrono::milliseconds(
                (kFramesPerChunk * 1000) / format.sample_rate));
        }
        queued_before_flush = transport.queued_packets().size();
        engine.flush();
    } catch (const std::exception& e) {
        print_sessions(transport, "write-failed");
        std::cerr << "FAIL write: " << e.what() << "\n";
        engine.stop();
        return false;
    }

    print_sessions(transport, "after-tone");
    std::cout << "AUDIO chunks=" << chunks << " queued_packets_before_flush=" << queued_before_flush
              << " duration_ms=" << options.duration.count()
              << " frequency_hz=" << options.frequency
              << " volume=" << options.volume << "\n";
    engine.stop();
    return chunks > 0 && queued_before_flush > 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        multiroom::airplay::AirPlayTransport transport;
        return run_audio_diagnostic(transport, options) ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
