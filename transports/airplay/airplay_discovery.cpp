#include "airplay_discovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <stdexcept>
#include <set>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windns.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace multiroom::airplay {

namespace {

constexpr uint16_t kDnsTypePtr = 12;
constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsTypeAaaa = 28;
constexpr uint16_t kDnsTypeTxt = 16;
constexpr uint16_t kDnsTypeSrv = 33;
constexpr uint16_t kDnsClassIn = 1;
constexpr uint16_t kDnsClassUnicastResponse = 0x8000;
constexpr const char* kAirPlayService = "_airplay._tcp.local";
constexpr const char* kRaopService = "_raop._tcp.local";

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

bool ends_with_service(const std::string& name, const char* service) {
    const auto lowered_name = lower_ascii(trim_trailing_dot(name));
    const auto lowered_service = lower_ascii(service);
    if (lowered_name.size() <= lowered_service.size() + 1) return false;
    return lowered_name.compare(lowered_name.size() - lowered_service.size(), lowered_service.size(), lowered_service) == 0 &&
           lowered_name[lowered_name.size() - lowered_service.size() - 1] == '.';
}

std::string infer_service_type(const std::string& full_name) {
    if (ends_with_service(full_name, kAirPlayService)) return kAirPlayService;
    if (ends_with_service(full_name, kRaopService)) return kRaopService;
    return {};
}

std::string service_instance_name(const std::string& full_name, const std::string& service_type) {
    const auto normalized_name = trim_trailing_dot(full_name);
    if (normalized_name.size() > service_type.size() + 1 &&
        lower_ascii(normalized_name).compare(normalized_name.size() - service_type.size(), service_type.size(), lower_ascii(service_type)) == 0) {
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
        lower_ascii(normalized_name).compare(normalized_name.size() - service_type.size(), service_type.size(), lower_ascii(service_type)) == 0) {
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

uint16_t read_u16(const std::vector<uint8_t>& packet, size_t offset) {
    if (offset + 2 > packet.size()) throw std::out_of_range("DNS packet is truncated.");
    return static_cast<uint16_t>((packet[offset] << 8) | packet[offset + 1]);
}

uint32_t read_u32(const std::vector<uint8_t>& packet, size_t offset) {
    if (offset + 4 > packet.size()) throw std::out_of_range("DNS packet is truncated.");
    return (static_cast<uint32_t>(packet[offset]) << 24) |
           (static_cast<uint32_t>(packet[offset + 1]) << 16) |
           (static_cast<uint32_t>(packet[offset + 2]) << 8) |
           static_cast<uint32_t>(packet[offset + 3]);
}

void append_dns_name(std::vector<uint8_t>& packet, const std::string& name) {
    const auto normalized_name = trim_trailing_dot(name);
    const char* label = normalized_name.c_str();
    while (*label != '\0') {
        const char* dot = std::strchr(label, '.');
        const auto length = dot != nullptr ? static_cast<size_t>(dot - label) : std::strlen(label);
        if (length == 0 || length > 63) throw std::invalid_argument("Invalid mDNS service name.");
        packet.push_back(static_cast<uint8_t>(length));
        packet.insert(packet.end(), label, label + length);
        if (dot == nullptr) break;
        label = dot + 1;
    }
    packet.push_back(0);
}

std::vector<uint8_t> make_dns_query(const std::string& name, uint16_t type, bool request_unicast_response) {
    std::vector<uint8_t> packet(12, 0);
    packet[5] = 1;
    append_dns_name(packet, name);
    packet.push_back(static_cast<uint8_t>(type >> 8));
    packet.push_back(static_cast<uint8_t>(type & 0xFF));
    const uint16_t question_class = request_unicast_response ? (kDnsClassIn | kDnsClassUnicastResponse) : kDnsClassIn;
    packet.push_back(static_cast<uint8_t>(question_class >> 8));
    packet.push_back(static_cast<uint8_t>(question_class & 0xFF));
    return packet;
}

class PacketReader {
public:
    explicit PacketReader(const std::vector<uint8_t>& packet) : packet_(packet) {}

    bool read_name(size_t& offset, std::string& out, int depth = 0) const {
        if (depth > 12) return false;
        size_t cursor = offset;
        bool jumped = false;
        out.clear();

        while (cursor < packet_.size()) {
            const uint8_t length = packet_[cursor++];
            if (length == 0) {
                if (!jumped) offset = cursor;
                return true;
            }

            if ((length & 0xC0) == 0xC0) {
                if (cursor >= packet_.size()) return false;
                const auto pointer = static_cast<size_t>(((length & 0x3F) << 8) | packet_[cursor++]);
                if (pointer >= packet_.size()) return false;
                if (!jumped) offset = cursor;
                cursor = pointer;
                jumped = true;
                ++depth;
                if (depth > 12) return false;
                continue;
            }

            if ((length & 0xC0) != 0 || cursor + length > packet_.size()) return false;
            if (!out.empty()) out.push_back('.');
            out.append(reinterpret_cast<const char*>(packet_.data() + cursor), length);
            cursor += length;
        }

        return false;
    }

private:
    const std::vector<uint8_t>& packet_;
};

struct DiscoveredService {
    std::string service_type;
    std::string full_name;
    std::string instance_name;
    std::string target_host;
    std::string target_address;
    uint16_t port = 0;
    std::map<std::string, std::string> txt;
};

std::map<std::string, std::string> parse_txt(const std::vector<uint8_t>& packet, size_t offset, size_t length) {
    std::map<std::string, std::string> result;
    const size_t end = offset + length;
    while (offset < end) {
        const uint8_t item_length = packet[offset++];
        if (offset + item_length > end) break;

        std::string item(reinterpret_cast<const char*>(packet.data() + offset), item_length);
        offset += item_length;

        const auto separator = item.find('=');
        const auto key = lower_ascii(separator == std::string::npos ? item : item.substr(0, separator));
        const auto value = separator == std::string::npos ? std::string{} : item.substr(separator + 1);
        if (!key.empty()) result[key] = value;
    }
    return result;
}

std::string ipv4_address_from_rdata(const std::vector<uint8_t>& packet, size_t offset, size_t length) {
    if (length != 4 || offset + length > packet.size()) {
        return {};
    }

    char text[INET_ADDRSTRLEN] = {};
    in_addr address = {};
    std::memcpy(&address, packet.data() + offset, sizeof(address));
    return inet_ntop(AF_INET, &address, text, sizeof(text)) == nullptr ? std::string{} : std::string{text};
}

std::string ipv6_address_from_rdata(const std::vector<uint8_t>& packet, size_t offset, size_t length) {
    if (length != 16 || offset + length > packet.size()) {
        return {};
    }

    char text[INET6_ADDRSTRLEN] = {};
    in6_addr address = {};
    std::memcpy(&address, packet.data() + offset, sizeof(address));
    return inet_ntop(AF_INET6, &address, text, sizeof(text)) == nullptr ? std::string{} : std::string{text};
}

void apply_host_addresses(
    std::map<std::string, DiscoveredService>& services,
    const std::map<std::string, std::string>& host_addresses) {
    for (auto& [_, service] : services) {
        if (service.target_host.empty()) {
            continue;
        }
        const auto it = host_addresses.find(lower_ascii(service.target_host));
        if (it != host_addresses.end()) {
            service.target_address = it->second;
        }
    }
}

void parse_response(
    const std::vector<uint8_t>& packet,
    std::map<std::string, DiscoveredService>& services,
    std::map<std::string, std::string>& host_addresses) {
    if (packet.size() < 12) return;

    const auto qdcount = read_u16(packet, 4);
    const auto ancount = read_u16(packet, 6);
    const auto nscount = read_u16(packet, 8);
    const auto arcount = read_u16(packet, 10);
    PacketReader reader(packet);

    size_t offset = 12;
    std::string name;
    for (uint16_t index = 0; index < qdcount; ++index) {
        if (!reader.read_name(offset, name) || offset + 4 > packet.size()) return;
        offset += 4;
    }

    const auto rr_count = static_cast<uint32_t>(ancount) + nscount + arcount;
    for (uint32_t index = 0; index < rr_count; ++index) {
        if (!reader.read_name(offset, name) || offset + 10 > packet.size()) return;
        const auto type = read_u16(packet, offset);
        offset += 2;
        const auto rr_class = read_u16(packet, offset);
        offset += 2;
        static_cast<void>(read_u32(packet, offset));
        offset += 4;
        const auto rdlength = read_u16(packet, offset);
        offset += 2;
        if ((rr_class & 0x7FFF) != kDnsClassIn || offset + rdlength > packet.size()) {
            offset += rdlength;
            continue;
        }

        const auto rdata_offset = offset;
        if (type == kDnsTypePtr) {
            std::string target;
            size_t target_offset = rdata_offset;
            if (reader.read_name(target_offset, target)) {
                auto& service = services[target];
                service.full_name = target;
                service.service_type = name;
                service.instance_name = service_instance_name(target, name);
            }
        } else if (type == kDnsTypeSrv) {
            if (rdlength >= 6) {
                auto& service = services[name];
                service.full_name = name;
                service.port = read_u16(packet, rdata_offset + 4);
                size_t target_offset = rdata_offset + 6;
                reader.read_name(target_offset, service.target_host);
            }
        } else if (type == kDnsTypeTxt) {
            auto& service = services[name];
            service.full_name = name;
            auto txt = parse_txt(packet, rdata_offset, rdlength);
            service.txt.insert(txt.begin(), txt.end());
        } else if (type == kDnsTypeA) {
            auto address = ipv4_address_from_rdata(packet, rdata_offset, rdlength);
            if (!address.empty()) {
                host_addresses[lower_ascii(name)] = std::move(address);
            }
        } else if (type == kDnsTypeAaaa) {
            auto address = ipv6_address_from_rdata(packet, rdata_offset, rdlength);
            if (!address.empty() && host_addresses.find(lower_ascii(name)) == host_addresses.end()) {
                host_addresses[lower_ascii(name)] = std::move(address);
            }
        }

        offset += rdlength;
    }

    apply_host_addresses(services, host_addresses);
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
        service.instance_name = service_instance_name(service.full_name, service.service_type);
    }
}

std::string device_id_from(const DiscoveredService& service) {
    auto id = txt_value(service.txt, "pk");
    if (id.empty()) {
        id = txt_value(service.txt, "deviceid");
        if (!id.empty()) id = normalize_hardware_id(std::move(id));
    }
    if (id.empty()) {
        auto instance = service_instance_prefix(service.full_name, service.service_type);
        const auto at = instance.find('@');
        if (at != std::string::npos && at > 0) {
            id = normalize_hardware_id(instance.substr(0, at));
        }
    }
    if (id.empty()) id = service.full_name;
    return "airplay:" + lower_ascii(id);
}

OutputDevice to_output_device(const DiscoveredService& service) {
    auto normalized_service = service;
    normalize_service(normalized_service);

    OutputDevice device;
    device.id = device_id_from(normalized_service);
    device.name = normalized_service.instance_name.empty() ? normalized_service.full_name : normalized_service.instance_name;
    device.type = OutputType::AirPlay;
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
    device.endpoint_host = normalized_service.target_address.empty() ? normalized_service.target_host : normalized_service.target_address;
    device.endpoint_port = normalized_service.port;
    device.txt_records = normalized_service.txt;
    return device;
}

#ifdef _WIN32
class WinsockSession {
public:
    WinsockSession() {
        WSADATA data = {};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed.");
        }
    }

    ~WinsockSession() {
        WSACleanup();
    }
};

std::string narrow_utf16(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};

    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

std::string narrow_dns_string(const wchar_t* text) {
    return narrow_utf16(text);
}

std::string narrow_dns_string(const char* text) {
    return text == nullptr ? std::string{} : std::string{text};
}

std::wstring widen_ascii(const char* text) {
    std::wstring result;
    if (text == nullptr) return result;
    while (*text != '\0') {
        result.push_back(static_cast<wchar_t>(*text++));
    }
    return result;
}

std::wstring dns_name_to_wstring(const wchar_t* text) {
    return text == nullptr ? std::wstring{} : std::wstring{text};
}

std::wstring dns_name_to_wstring(const char* text) {
    return widen_ascii(text);
}

std::string ipv4_address_from_dns_service(const IP4_ADDRESS* address) {
    if (address == nullptr) return {};

    char text[INET_ADDRSTRLEN] = {};
    in_addr ipv4 = {};
    ipv4.S_un.S_addr = *address;
    return inet_ntop(AF_INET, &ipv4, text, sizeof(text)) == nullptr ? std::string{} : std::string{text};
}

void merge_device(OutputDevice& target, const OutputDevice& candidate) {
    if (target.name.empty()) {
        target.name = candidate.name;
    }
    if (target.endpoint_host.empty() && !candidate.endpoint_host.empty()) {
        target.endpoint_host = candidate.endpoint_host;
    }
    if (target.endpoint_port == 0 && candidate.endpoint_port != 0) {
        target.endpoint_port = candidate.endpoint_port;
    }

    target.has_password = target.has_password || candidate.has_password;
    target.requires_auth = target.requires_auth || candidate.requires_auth;
    target.needs_auth_key = target.needs_auth_key || candidate.needs_auth_key;
    target.supports_airplay2 = target.supports_airplay2 || candidate.supports_airplay2;
    target.supports_legacy_l16 = target.supports_legacy_l16 || candidate.supports_legacy_l16;
    target.requires_encrypted_stream = target.requires_encrypted_stream || candidate.requires_encrypted_stream;

    for (const auto& format : candidate.supported_formats) {
        if (std::find(target.supported_formats.begin(), target.supported_formats.end(), format) == target.supported_formats.end()) {
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

void append_or_merge_device(std::vector<OutputDevice>& devices, OutputDevice candidate) {
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return device.id == candidate.id;
    });
    if (it == devices.end()) {
        devices.push_back(std::move(candidate));
        return;
    }

    merge_device(*it, candidate);
}

struct BrowseContext {
    std::mutex mutex;
    std::condition_variable changed;
    std::set<std::wstring> service_names;
    bool cancelled = false;
};

struct ResolveContext {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<DiscoveredService> service;
    bool complete = false;
};

void WINAPI dns_service_browse_callback(DWORD status, PVOID query_context, PDNS_RECORD record_list) {
    auto* context = static_cast<BrowseContext*>(query_context);
    if (context == nullptr) return;

    {
        std::lock_guard lock(context->mutex);
        if (status == ERROR_CANCELLED) {
            context->cancelled = true;
        } else if (status == ERROR_SUCCESS) {
            for (auto* record = record_list; record != nullptr; record = record->pNext) {
                if (record->wType == DNS_TYPE_PTR && record->Data.PTR.pNameHost != nullptr) {
                    context->service_names.insert(dns_name_to_wstring(record->Data.PTR.pNameHost));
                }
            }
        }
    }

    if (record_list != nullptr) {
        DnsRecordListFree(record_list, DnsFreeRecordList);
    }
    context->changed.notify_all();
}

void WINAPI dns_service_resolve_callback(DWORD status, PVOID query_context, PDNS_SERVICE_INSTANCE instance) {
    auto* context = static_cast<ResolveContext*>(query_context);
    if (context == nullptr) return;

    {
        std::lock_guard lock(context->mutex);
        if (status == ERROR_SUCCESS && instance != nullptr) {
            DiscoveredService service;
            service.full_name = trim_trailing_dot(narrow_dns_string(instance->pszInstanceName));
            service.instance_name = service_instance_name(service.full_name, service.full_name.find(kAirPlayService) != std::string::npos ? kAirPlayService : kRaopService);
            service.target_host = trim_trailing_dot(narrow_dns_string(instance->pszHostName));
            service.target_address = ipv4_address_from_dns_service(instance->ip4Address);
            service.port = instance->wPort;

            for (DWORD index = 0; index < instance->dwPropertyCount; ++index) {
                const auto key = lower_ascii(narrow_dns_string(instance->keys[index]));
                const auto value = narrow_dns_string(instance->values[index]);
                if (!key.empty()) service.txt[key] = value;
            }

            context->service = std::move(service);
        }
        context->complete = true;
    }

    if (instance != nullptr) {
        DnsServiceFreeInstance(instance);
    }
    context->changed.notify_all();
}

std::optional<DiscoveredService> resolve_windows_dns_sd_service(const std::wstring& service_name, std::chrono::milliseconds timeout) {
    ResolveContext context;
    DNS_SERVICE_CANCEL cancel = {};
    DNS_SERVICE_RESOLVE_REQUEST request = {};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.InterfaceIndex = 0;
    request.QueryName = const_cast<PWSTR>(service_name.c_str());
    request.pResolveCompletionCallback = dns_service_resolve_callback;
    request.pQueryContext = &context;

    const auto status = DnsServiceResolve(&request, &cancel);
    if (status != DNS_REQUEST_PENDING) {
        return std::nullopt;
    }

    {
        std::unique_lock lock(context.mutex);
        context.changed.wait_for(lock, timeout, [&]() { return context.complete; });
    }

    if (!context.complete) {
        DnsServiceResolveCancel(&cancel);
        std::unique_lock lock(context.mutex);
        context.changed.wait_for(lock, std::chrono::milliseconds(250), [&]() { return context.complete; });
    }

    return context.service;
}

std::vector<OutputDevice> browse_windows_dns_sd(std::chrono::milliseconds timeout) {
    std::vector<OutputDevice> devices;
    std::set<std::string> seen_ids;

    for (const auto* service_type : {kAirPlayService, kRaopService}) {
        BrowseContext context;
        DNS_SERVICE_CANCEL cancel = {};
        DNS_SERVICE_BROWSE_REQUEST request = {};
        const auto query_wide = widen_ascii(service_type);
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex = 0;
        request.QueryName = query_wide.c_str();
        request.pBrowseCallback = dns_service_browse_callback;
        request.pQueryContext = &context;

        const auto status = DnsServiceBrowse(&request, &cancel);
        if (status != DNS_REQUEST_PENDING) {
            continue;
        }

        {
            std::unique_lock lock(context.mutex);
            context.changed.wait_for(lock, timeout);
        }

        DnsServiceBrowseCancel(&cancel);
        {
            std::unique_lock lock(context.mutex);
            context.changed.wait_for(lock, std::chrono::milliseconds(250), [&]() { return context.cancelled; });
        }

        std::vector<std::wstring> service_names;
        {
            std::lock_guard lock(context.mutex);
            service_names.assign(context.service_names.begin(), context.service_names.end());
        }

        for (const auto& service_name : service_names) {
            auto resolved = resolve_windows_dns_sd_service(service_name, timeout);
            if (!resolved) continue;

            resolved->service_type = service_type;
            normalize_service(*resolved);

            auto device = to_output_device(*resolved);
            if (seen_ids.insert(device.id).second) {
                devices.push_back(std::move(device));
            } else {
                append_or_merge_device(devices, std::move(device));
            }
        }
    }

    return devices;
}

std::vector<in_addr> local_ipv4_interfaces() {
    std::vector<in_addr> result;

    ULONG buffer_size = 15 * 1024;
    std::vector<uint8_t> buffer(buffer_size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG rc = GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr,
        adapters,
        &buffer_size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            adapters,
            &buffer_size);
    }
    if (rc != NO_ERROR) return result;

    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if ((adapter->Flags & IP_ADAPTER_NO_MULTICAST) != 0) continue;

        for (auto* address = adapter->FirstUnicastAddress; address != nullptr; address = address->Next) {
            if (address->Address.lpSockaddr == nullptr ||
                address->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }

            auto* ipv4 = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);
            if (ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK) ||
                ipv4->sin_addr.s_addr == htonl(INADDR_ANY)) {
                continue;
            }
            result.push_back(ipv4->sin_addr);
        }
    }

    return result;
}

void join_mdns_multicast(SOCKET socket_handle) {
    const auto interfaces = local_ipv4_interfaces();
    if (interfaces.empty()) {
        ip_mreq request = {};
        inet_pton(AF_INET, "224.0.0.251", &request.imr_multiaddr);
        request.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(socket_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&request), sizeof(request));
        return;
    }

    for (const auto& interface_address : interfaces) {
        ip_mreq request = {};
        inet_pton(AF_INET, "224.0.0.251", &request.imr_multiaddr);
        request.imr_interface = interface_address;
        setsockopt(socket_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&request), sizeof(request));
    }
}

void send_mdns_query(
    SOCKET socket_handle,
    const std::vector<uint8_t>& query,
    const sockaddr_in& multicast,
    const std::vector<in_addr>& interfaces) {
    if (interfaces.empty()) {
        sendto(socket_handle,
               reinterpret_cast<const char*>(query.data()),
               static_cast<int>(query.size()),
               0,
               reinterpret_cast<const sockaddr*>(&multicast),
               sizeof(multicast));
        return;
    }

    for (const auto& interface_address : interfaces) {
        setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&interface_address), sizeof(interface_address));
        sendto(socket_handle,
               reinterpret_cast<const char*>(query.data()),
               static_cast<int>(query.size()),
               0,
               reinterpret_cast<const sockaddr*>(&multicast),
               sizeof(multicast));
    }
}

void receive_mdns_responses_until(
    SOCKET socket_handle,
    std::chrono::steady_clock::time_point deadline,
    std::map<std::string, DiscoveredService>& services,
    std::map<std::string, std::string>& host_addresses) {
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<uint8_t, 9000> buffer = {};
        sockaddr_in from = {};
        int from_len = sizeof(from);
        const int received = recvfrom(
            socket_handle,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_len);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                continue;
            }
            break;
        }
        if (received > 0) {
            try {
                parse_response(std::vector<uint8_t>(buffer.begin(), buffer.begin() + received), services, host_addresses);
            } catch (const std::exception&) {
            }
        }
    }
}

std::vector<std::string> discovered_service_names(const std::map<std::string, DiscoveredService>& services) {
    std::set<std::string> names;
    for (const auto& [key, service] : services) {
        const auto full_name = trim_trailing_dot(service.full_name.empty() ? key : service.full_name);
        if (!full_name.empty() &&
            (ends_with_service(full_name, kAirPlayService) || ends_with_service(full_name, kRaopService))) {
            names.insert(full_name);
        }
    }
    return {names.begin(), names.end()};
}

std::vector<std::string> unresolved_host_names(
    const std::map<std::string, DiscoveredService>& services,
    const std::map<std::string, std::string>& host_addresses) {
    std::set<std::string> names;
    for (const auto& [_, service] : services) {
        const auto host = trim_trailing_dot(service.target_host);
        if (!host.empty() && host_addresses.find(lower_ascii(host)) == host_addresses.end()) {
            names.insert(host);
        }
    }
    return {names.begin(), names.end()};
}

std::vector<OutputDevice> browse_raw_mdns(std::chrono::milliseconds timeout) {
    WinsockSession winsock;
    const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        throw std::runtime_error("Could not create mDNS UDP socket.");
    }

    const auto close_socket = [&]() {
        closesocket(socket_handle);
    };

    auto receive_timeout_value = timeout.count() / 4;
    if (receive_timeout_value < 25) {
        receive_timeout_value = 25;
    } else if (receive_timeout_value > 250) {
        receive_timeout_value = 250;
    }
    DWORD receive_timeout_ms = static_cast<DWORD>(receive_timeout_value);
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receive_timeout_ms), sizeof(receive_timeout_ms));
    BOOL reuse_addr = TRUE;
    setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse_addr), sizeof(reuse_addr));
    DWORD ttl = 255;
    setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(5353);
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
        bind_addr.sin_port = 0;
        if (bind(socket_handle, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
            close_socket();
            throw std::runtime_error("Could not bind mDNS UDP socket.");
        }
    } else {
        join_mdns_multicast(socket_handle);
    }

    sockaddr_in multicast = {};
    multicast.sin_family = AF_INET;
    multicast.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &multicast.sin_addr);
    const auto interfaces = local_ipv4_interfaces();

    sockaddr_in local_addr = {};
    int local_addr_len = sizeof(local_addr);
    const bool request_unicast_response =
        getsockname(socket_handle, reinterpret_cast<sockaddr*>(&local_addr), &local_addr_len) == 0 &&
        ntohs(local_addr.sin_port) != 5353;

    for (const auto* service : {kAirPlayService, kRaopService}) {
        const auto query = make_dns_query(service, kDnsTypePtr, request_unicast_response);
        send_mdns_query(socket_handle, query, multicast, interfaces);
    }

    std::map<std::string, DiscoveredService> services;
    std::map<std::string, std::string> host_addresses;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + timeout;
    const auto phase = (std::max)(std::chrono::milliseconds(150), timeout / 3);

    receive_mdns_responses_until(socket_handle, (std::min)(start + phase, deadline), services, host_addresses);

    for (const auto& service_name : discovered_service_names(services)) {
        send_mdns_query(socket_handle, make_dns_query(service_name, kDnsTypeSrv, request_unicast_response), multicast, interfaces);
        send_mdns_query(socket_handle, make_dns_query(service_name, kDnsTypeTxt, request_unicast_response), multicast, interfaces);
    }

    receive_mdns_responses_until(socket_handle, (std::min)(start + (phase * 2), deadline), services, host_addresses);

    for (const auto& host_name : unresolved_host_names(services, host_addresses)) {
        send_mdns_query(socket_handle, make_dns_query(host_name, kDnsTypeA, request_unicast_response), multicast, interfaces);
        send_mdns_query(socket_handle, make_dns_query(host_name, kDnsTypeAaaa, request_unicast_response), multicast, interfaces);
    }

    receive_mdns_responses_until(socket_handle, deadline, services, host_addresses);

    close_socket();

    std::vector<OutputDevice> devices;
    for (const auto& [_, service] : services) {
        auto normalized_service = service;
        normalize_service(normalized_service);
        if (!ends_with_service(normalized_service.full_name, kAirPlayService) &&
            !ends_with_service(normalized_service.full_name, kRaopService) &&
            !ends_with_service(normalized_service.service_type, kAirPlayService) &&
            !ends_with_service(normalized_service.service_type, kRaopService)) {
            continue;
        }
        if (normalized_service.full_name.empty()) continue;
        append_or_merge_device(devices, to_output_device(normalized_service));
    }

    return devices;
}

std::vector<OutputDevice> browse_mdns(std::chrono::milliseconds timeout) {
    auto devices = browse_windows_dns_sd(timeout);
    try {
        auto raw_devices = browse_raw_mdns(timeout);
        for (auto& device : raw_devices) {
            append_or_merge_device(devices, std::move(device));
        }
    } catch (const std::exception&) {
        if (devices.empty()) {
            throw;
        }
    }

    return devices;
}
#else
std::vector<OutputDevice> browse_mdns(std::chrono::milliseconds) {
    return {};
}
#endif

}  // namespace

void AirPlayDiscovery::start() {
    std::lock_guard lock(mutex_);
    active_ = true;
}

void AirPlayDiscovery::stop() {
    std::lock_guard lock(mutex_);
    active_ = false;
}

bool AirPlayDiscovery::active() const {
    std::lock_guard lock(mutex_);
    return active_;
}

void AirPlayDiscovery::refresh(std::chrono::milliseconds timeout) {
    if (!active()) return;

    auto devices = browse_mdns(timeout);
    std::lock_guard lock(mutex_);
    for (auto& device : devices) {
        devices_[device.id] = std::move(device);
    }
}

void AirPlayDiscovery::upsert(OutputDevice device) {
    if (device.id.empty()) {
        throw std::invalid_argument("Discovered AirPlay device id cannot be empty.");
    }

    device.type = OutputType::AirPlay;

    std::lock_guard lock(mutex_);
    devices_[device.id] = std::move(device);
}

std::vector<OutputDevice> AirPlayDiscovery::list() const {
    std::lock_guard lock(mutex_);

    std::vector<OutputDevice> result;
    result.reserve(devices_.size());
    for (const auto& [_, device] : devices_) {
        result.push_back(device);
    }

    return result;
}

std::optional<OutputDevice> AirPlayDiscovery::find(const std::string& id) const {
    std::lock_guard lock(mutex_);

    const auto it = devices_.find(id);
    if (it == devices_.end()) {
        return std::nullopt;
    }

    return it->second;
}

}  // namespace multiroom::airplay
