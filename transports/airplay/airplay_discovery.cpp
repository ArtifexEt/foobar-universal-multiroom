#include "airplay_discovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace multiroom::airplay {

namespace {

constexpr uint16_t kDnsTypePtr = 12;
constexpr uint16_t kDnsTypeTxt = 16;
constexpr uint16_t kDnsTypeSrv = 33;
constexpr uint16_t kDnsClassIn = 1;
constexpr const char* kAirPlayService = "_airplay._tcp.local";
constexpr const char* kRaopService = "_raop._tcp.local";

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ends_with_service(const std::string& name, const char* service) {
    const auto lowered_name = lower_ascii(name);
    const auto lowered_service = lower_ascii(service);
    if (lowered_name.size() <= lowered_service.size() + 1) return false;
    return lowered_name.compare(lowered_name.size() - lowered_service.size(), lowered_service.size(), lowered_service) == 0 &&
           lowered_name[lowered_name.size() - lowered_service.size() - 1] == '.';
}

std::string service_instance_name(const std::string& full_name, const std::string& service_type) {
    if (full_name.size() > service_type.size() + 1 &&
        lower_ascii(full_name).compare(full_name.size() - service_type.size(), service_type.size(), lower_ascii(service_type)) == 0) {
        auto instance = full_name.substr(0, full_name.size() - service_type.size() - 1);
        const auto at = instance.find('@');
        if (at != std::string::npos && at + 1 < instance.size()) {
            instance.erase(0, at + 1);
        }
        return instance;
    }
    return full_name;
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

void append_dns_name(std::vector<uint8_t>& packet, const char* name) {
    const char* label = name;
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

std::vector<uint8_t> make_ptr_query(const char* service) {
    std::vector<uint8_t> packet(12, 0);
    packet[5] = 1;
    append_dns_name(packet, service);
    packet.push_back(0);
    packet.push_back(static_cast<uint8_t>(kDnsTypePtr));
    packet.push_back(0);
    packet.push_back(static_cast<uint8_t>(kDnsClassIn));
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

void parse_response(const std::vector<uint8_t>& packet, std::map<std::string, DiscoveredService>& services) {
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
        }

        offset += rdlength;
    }
}

std::string txt_value(const std::map<std::string, std::string>& txt, const char* key) {
    const auto it = txt.find(key);
    return it == txt.end() ? std::string{} : it->second;
}

bool truthy_txt(const std::map<std::string, std::string>& txt, const char* key) {
    const auto value = lower_ascii(txt_value(txt, key));
    return value == "1" || value == "true" || value == "yes";
}

std::string device_id_from(const DiscoveredService& service) {
    auto id = txt_value(service.txt, "deviceid");
    if (id.empty()) id = txt_value(service.txt, "pk");
    if (id.empty()) id = service.full_name;
    return "airplay:" + lower_ascii(id);
}

OutputDevice to_output_device(const DiscoveredService& service) {
    OutputDevice device;
    device.id = device_id_from(service);
    device.name = service.instance_name.empty() ? service.full_name : service.instance_name;
    device.type = OutputType::AirPlay;
    device.has_password = truthy_txt(service.txt, "pw");
    device.requires_auth = device.has_password || truthy_txt(service.txt, "requiresauth");
    device.needs_auth_key = truthy_txt(service.txt, "sf");
    device.volume = 50;
    device.format = "airplay";
    device.supported_formats = {"airplay"};
    device.endpoint_host = service.target_host;
    device.endpoint_port = service.port;
    device.txt_records = service.txt;
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

std::vector<OutputDevice> browse_mdns(std::chrono::milliseconds timeout) {
    WinsockSession winsock;
    const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        throw std::runtime_error("Could not create mDNS UDP socket.");
    }

    const auto close_socket = [&]() {
        closesocket(socket_handle);
    };

    DWORD timeout_ms = static_cast<DWORD>(std::max<int64_t>(1, timeout.count()));
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = 0;
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
        close_socket();
        throw std::runtime_error("Could not bind mDNS UDP socket.");
    }

    sockaddr_in multicast = {};
    multicast.sin_family = AF_INET;
    multicast.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &multicast.sin_addr);

    for (const auto* service : {kAirPlayService, kRaopService}) {
        const auto query = make_ptr_query(service);
        sendto(socket_handle,
               reinterpret_cast<const char*>(query.data()),
               static_cast<int>(query.size()),
               0,
               reinterpret_cast<sockaddr*>(&multicast),
               sizeof(multicast));
    }

    std::map<std::string, DiscoveredService> services;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
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
        if (received == SOCKET_ERROR) break;
        if (received > 0) {
            try {
                parse_response(std::vector<uint8_t>(buffer.begin(), buffer.begin() + received), services);
            } catch (const std::exception&) {
            }
        }
    }

    close_socket();

    std::vector<OutputDevice> devices;
    for (const auto& [_, service] : services) {
        if (!ends_with_service(service.full_name, kAirPlayService) &&
            !ends_with_service(service.full_name, kRaopService) &&
            !ends_with_service(service.service_type, kAirPlayService) &&
            !ends_with_service(service.service_type, kRaopService)) {
            continue;
        }
        if (service.full_name.empty()) continue;
        devices.push_back(to_output_device(service));
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
