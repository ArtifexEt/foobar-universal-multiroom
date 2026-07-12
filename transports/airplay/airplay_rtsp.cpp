#include "airplay_rtsp.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "airplay2_external_crypto.h"

#include <airplay_crypto.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <limits>
#include <atomic>
#include <thread>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace multiroom::airplay {

namespace {

constexpr uint8_t kRtpPayloadTypeDynamicL16 = 96;
constexpr uint8_t kRtpPayloadTypeAirPlay2Realtime = 0x60;
constexpr size_t kAirPlay2FramesPerPacket = 352;
constexpr size_t kAirPlay2Channels = 2;
constexpr uint32_t kAirPlay2SampleRate = 44100;
constexpr uint32_t kAirPlay2LatencyFrames = 88200;
constexpr uint32_t kAirPlay2MinimumLatencyFrames = 11025;
constexpr uint32_t kDefaultSsrc = 0x46424d52;  // FBMR

enum class AirPlay2AudioEncoding {
    Alac,
    Pcm,
};

#ifdef _WIN32
using socket_handle_t = SOCKET;
constexpr socket_handle_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_handle_t = int;
constexpr socket_handle_t kInvalidSocket = -1;
#endif

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim_ascii(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string map_value(const std::map<std::string, std::string>& values, const char* key) {
    const auto it = values.find(key);
    return it == values.end() ? std::string{} : it->second;
}

bool contains_lower(std::string value, const char* needle) {
    value = lower_ascii(std::move(value));
    return value.find(needle) != std::string::npos;
}

AirPlay2AudioEncoding preferred_airplay2_audio_encoding(const OutputDevice& output) {
    if (contains_lower(map_value(output.txt_records, "manufacturer"), "linkplay") ||
        contains_lower(map_value(output.txt_records, "model"), "wiim") ||
        contains_lower(map_value(output.txt_records, "am"), "wiim") ||
        contains_lower(output.name, "wiim")) {
        return AirPlay2AudioEncoding::Pcm;
    }
    return AirPlay2AudioEncoding::Alac;
}

std::vector<std::string> split_methods(const std::string& methods) {
    std::vector<std::string> result;
    std::stringstream stream(methods);
    std::string method;
    while (std::getline(stream, method, ',')) {
        method = trim_ascii(method);
        if (!method.empty()) {
            result.push_back(method);
        }
    }
    return result;
}

void close_socket(socket_handle_t handle) {
    if (handle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(handle);
#else
    close(handle);
#endif
}

void write_u16_be(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void write_u32_be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

std::vector<uint8_t> bytes_from_string(const std::string& value) {
    return std::vector<uint8_t>(value.begin(), value.end());
}

std::string string_from_bytes(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

uint32_t random_u32() {
    const auto bytes = fxchain::airplay::randomBytes(4);
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

std::string random_hex(size_t bytes_count) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    const auto bytes = fxchain::airplay::randomBytes(bytes_count);
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(kHex[(byte >> 4) & 0x0F]);
        result.push_back(kHex[byte & 0x0F]);
    }
    return result;
}

std::string random_uuid_text() {
    auto bytes = fxchain::airplay::randomBytes(16);
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(36);
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        result.push_back(kHex[(bytes[index] >> 4) & 0x0F]);
        result.push_back(kHex[bytes[index] & 0x0F]);
    }
    return result;
}

std::string colon_hex_text(const std::string& hex) {
    std::ostringstream stream;
    for (size_t index = 0; index + 1 < hex.size(); index += 2) {
        if (index != 0) {
            stream << ':';
        }
        stream << hex[index] << hex[index + 1];
    }
    return stream.str();
}

uint64_t ntp_now() {
    using namespace std::chrono;
    constexpr uint64_t kUnixToNtpSeconds = 2208988800ULL;
    const auto now = system_clock::now().time_since_epoch();
    const auto seconds = duration_cast<std::chrono::seconds>(now);
    const auto fraction_ns = duration_cast<std::chrono::nanoseconds>(now - seconds).count();
    const auto fraction = (static_cast<unsigned long long>(fraction_ns) << 32) / 1000000000ULL;
    return ((static_cast<uint64_t>(seconds.count()) + kUnixToNtpSeconds) << 32) | fraction;
}

uint64_t ntp_to_rtp_timestamp(uint64_t ntp, uint32_t sample_rate) {
    return ((ntp >> 16) * sample_rate) >> 16;
}

uint64_t rtp_timestamp_to_ntp(uint64_t rtp_timestamp, uint32_t sample_rate) {
    return ((rtp_timestamp << 16) / sample_rate) << 16;
}

std::vector<uint8_t> hkdf_sha512(
    const std::string& salt,
    const std::string& info,
    const std::vector<uint8_t>& input) {
    return fxchain::airplay::hkdfSha512(salt, info, input, 32);
}

std::vector<uint8_t> encrypt_with_named_nonce(
    const std::vector<uint8_t>& key,
    const char* nonce,
    const std::vector<uint8_t>& plaintext) {
    return fxchain::airplay::chacha20Poly1305Encrypt(key, bytes_from_string(nonce), plaintext, {});
}

std::vector<uint8_t> decrypt_with_named_nonce(
    const std::vector<uint8_t>& key,
    const char* nonce,
    const std::vector<uint8_t>& encrypted) {
    auto plaintext = fxchain::airplay::chacha20Poly1305Decrypt(key, bytes_from_string(nonce), encrypted, {});
    if (!plaintext) {
        throw std::runtime_error("AirPlay pairing encrypted message failed authentication.");
    }
    return *plaintext;
}

std::vector<uint8_t> make_rtp_header(
    uint8_t payload_type,
    bool marker,
    uint16_t sequence,
    uint32_t timestamp,
    uint32_t ssrc) {
    std::vector<uint8_t> packet;
    packet.reserve(12);
    packet.push_back(0x80);
    packet.push_back(static_cast<uint8_t>(payload_type | (marker ? 0x80 : 0x00)));
    write_u16_be(packet, sequence);
    write_u32_be(packet, timestamp);
    write_u32_be(packet, ssrc);
    return packet;
}

std::vector<uint8_t> make_alac_uncompressed_frame(const void* frames, size_t bytes) {
    std::array<int16_t, kAirPlay2FramesPerPacket * kAirPlay2Channels> samples = {};
    const auto samples_to_copy = std::min(samples.size(), bytes / sizeof(int16_t));
    const auto* input = static_cast<const uint8_t*>(frames);
    for (size_t index = 0; index < samples_to_copy; ++index) {
        const auto low = static_cast<uint16_t>(input[index * 2]);
        const auto high = static_cast<uint16_t>(input[index * 2 + 1]) << 8;
        samples[index] = static_cast<int16_t>(low | high);
    }

    std::vector<uint8_t> out;
    out.reserve(samples.size() * 2 + 8);
    uint8_t current = 0;
    int filled = 0;
    const auto put_bits = [&](uint32_t value, int bits) {
        for (int bit = bits - 1; bit >= 0; --bit) {
            current = static_cast<uint8_t>((current << 1) | ((value >> bit) & 1u));
            if (++filled == 8) {
                out.push_back(current);
                current = 0;
                filled = 0;
            }
        }
    };

    put_bits(1, 3);
    put_bits(0, 4);
    put_bits(0, 12);
    put_bits(0, 1);
    put_bits(0, 2);
    put_bits(1, 1);
    for (const auto sample : samples) {
        put_bits(static_cast<uint16_t>(sample), 16);
    }
    put_bits(7, 3);
    if (filled > 0) {
        current = static_cast<uint8_t>(current << (8 - filled));
        out.push_back(current);
    }
    return out;
}

std::vector<uint8_t> make_airplay2_pcm_payload(const void* frames, size_t bytes) {
    std::vector<uint8_t> out(kAirPlay2FramesPerPacket * kAirPlay2Channels * sizeof(int16_t), 0);
    const auto samples_to_copy = std::min(kAirPlay2FramesPerPacket * kAirPlay2Channels, bytes / sizeof(int16_t));
    const auto* input = static_cast<const uint8_t*>(frames);
    for (size_t index = 0; index < samples_to_copy; ++index) {
        out[index * 2] = input[index * 2 + 1];
        out[index * 2 + 1] = input[index * 2];
    }
    return out;
}

std::vector<uint8_t> encrypt_airplay2_audio_payload(
    const std::vector<uint8_t>& audio_key,
    uint64_t nonce,
    const std::vector<uint8_t>& rtp_header,
    const std::vector<uint8_t>& payload) {
    if (audio_key.size() != 32 || rtp_header.size() < 12) {
        throw std::logic_error("AirPlay 2 audio encryption is not initialized.");
    }

    const std::vector<uint8_t> aad(rtp_header.begin() + 4, rtp_header.begin() + 12);
    const auto nonce8 = fxchain::airplay::counterNonce8(nonce);
    auto encrypted = fxchain::airplay::chacha20Poly1305Encrypt(audio_key, nonce8, payload, aad);
    encrypted.insert(encrypted.end(), nonce8.begin(), nonce8.end());
    return encrypted;
}

std::vector<uint8_t> make_rtp_l16_packet(
    uint16_t sequence,
    uint32_t timestamp,
    uint32_t ssrc,
    const void* frames,
    size_t bytes) {
    if (bytes > std::numeric_limits<size_t>::max() - 12) {
        throw std::overflow_error("RTP packet size overflow.");
    }
    if ((bytes % 2) != 0) {
        throw std::invalid_argument("L16 RTP payload must contain whole 16-bit samples.");
    }

    std::vector<uint8_t> packet;
    packet.reserve(12 + bytes);
    packet.push_back(0x80);
    packet.push_back(kRtpPayloadTypeDynamicL16);
    write_u16_be(packet, sequence);
    write_u32_be(packet, timestamp);
    write_u32_be(packet, ssrc);

    const auto* samples = static_cast<const uint8_t*>(frames);
    for (size_t index = 0; index < bytes; index += 2) {
        packet.push_back(samples[index + 1]);
        packet.push_back(samples[index]);
    }

    return packet;
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
#endif

class AddrInfoList {
public:
    AddrInfoList(const std::string& host, uint16_t port, int socket_type) {
        addrinfo hints = {};
        hints.ai_socktype = socket_type;
        hints.ai_family = AF_UNSPEC;

        const auto port_text = std::to_string(port);
        const int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &head_);
        if (rc != 0 || head_ == nullptr) {
            throw std::runtime_error("Could not resolve AirPlay endpoint: " + host);
        }
    }

    ~AddrInfoList() {
        if (head_ != nullptr) {
            freeaddrinfo(head_);
        }
    }

    addrinfo* begin() const {
        return head_;
    }

private:
    addrinfo* head_ = nullptr;
};

class UdpSocket {
public:
    UdpSocket() = default;

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    UdpSocket(UdpSocket&& other) noexcept
        : handle_(other.handle_)
        , port_(other.port_) {
        other.handle_ = kInvalidSocket;
        other.port_ = 0;
    }

    UdpSocket& operator=(UdpSocket&& other) noexcept {
        if (this != &other) {
            close_socket(handle_);
            handle_ = other.handle_;
            port_ = other.port_;
            other.handle_ = kInvalidSocket;
            other.port_ = 0;
        }
        return *this;
    }

    ~UdpSocket() {
        close_socket(handle_);
    }

    static UdpSocket bind_ephemeral() {
        UdpSocket result;
        result.handle_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (result.handle_ == kInvalidSocket) {
            throw std::runtime_error("Could not create UDP socket for AirPlay transport.");
        }

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;

        if (bind(result.handle_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("Could not bind UDP socket for AirPlay transport.");
        }

        sockaddr_in bound = {};
#ifdef _WIN32
        int bound_size = sizeof(bound);
#else
        socklen_t bound_size = sizeof(bound);
#endif
        if (getsockname(result.handle_, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
            throw std::runtime_error("Could not inspect UDP socket for AirPlay transport.");
        }

        result.port_ = ntohs(bound.sin_port);
        return result;
    }

    uint16_t port() const {
        return port_;
    }

    void send_to(const std::string& host, uint16_t port, const std::vector<uint8_t>& bytes) {
        if (handle_ == kInvalidSocket) {
            throw std::logic_error("UDP socket is not open.");
        }
        if (port == 0) {
            throw std::invalid_argument("Remote UDP port cannot be zero.");
        }

        AddrInfoList addresses(host, port, SOCK_DGRAM);
        for (addrinfo* address = addresses.begin(); address != nullptr; address = address->ai_next) {
            if (address->ai_socktype != SOCK_DGRAM && address->ai_socktype != 0) {
                continue;
            }
#ifdef _WIN32
            const int sent = sendto(
                handle_,
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(bytes.size()),
                0,
                address->ai_addr,
                static_cast<int>(address->ai_addrlen));
#else
            const ssize_t sent = sendto(
                handle_,
                bytes.data(),
                bytes.size(),
                0,
                address->ai_addr,
                address->ai_addrlen);
#endif
            if (sent > 0 && static_cast<size_t>(sent) == bytes.size()) {
                return;
            }
        }

        throw std::runtime_error("Could not send RTP packet to AirPlay endpoint.");
    }

    bool receive_from(std::vector<uint8_t>& bytes, sockaddr_storage& from, int& from_size) {
        if (handle_ == kInvalidSocket) {
            return false;
        }

        bytes.assign(2048, 0);
        from = {};
#ifdef _WIN32
        from_size = sizeof(from);
        const int received = recvfrom(
            handle_,
            reinterpret_cast<char*>(bytes.data()),
            static_cast<int>(bytes.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_size);
#else
        socklen_t size = sizeof(from);
        const ssize_t received = recvfrom(
            handle_,
            bytes.data(),
            bytes.size(),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &size);
        from_size = static_cast<int>(size);
#endif
        if (received <= 0) {
            bytes.clear();
            return false;
        }

        bytes.resize(static_cast<size_t>(received));
        return true;
    }

    void send_to_address(const sockaddr_storage& to, int to_size, const std::vector<uint8_t>& bytes) {
        if (handle_ == kInvalidSocket || bytes.empty()) {
            return;
        }
#ifdef _WIN32
        sendto(
            handle_,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()),
            0,
            reinterpret_cast<const sockaddr*>(&to),
            to_size);
#else
        sendto(
            handle_,
            bytes.data(),
            bytes.size(),
            0,
            reinterpret_cast<const sockaddr*>(&to),
            static_cast<socklen_t>(to_size));
#endif
    }

    void set_receive_timeout(unsigned long timeout_ms) {
        if (handle_ == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        DWORD timeout = static_cast<DWORD>(timeout_ms);
        setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout = {};
        timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
        timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
        setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    }

private:
    socket_handle_t handle_ = kInvalidSocket;
    uint16_t port_ = 0;
};

struct LocalUdpPorts {
    UdpSocket data;
    UdpSocket control;
    UdpSocket timing;

    AirPlayTransportPorts to_transport_ports() const {
        AirPlayTransportPorts ports;
        ports.local_data_port = data.port();
        ports.local_control_port = control.port();
        ports.local_timing_port = timing.port();
        return ports;
    }
};

class AirPlay2StreamSetupRejected final : public std::runtime_error {
public:
    AirPlay2StreamSetupRejected(int status_code, const std::string& reason)
        : std::runtime_error(
              "AirPlay RTSP SETUP failed with status " +
              std::to_string(status_code) +
              (reason.empty() ? std::string{} : " " + reason))
        , status_code_(status_code) {}

    int status_code() const {
        return status_code_;
    }

private:
    int status_code_ = 0;
};

LocalUdpPorts bind_local_udp_ports() {
    return {
        UdpSocket::bind_ephemeral(),
        UdpSocket::bind_ephemeral(),
        UdpSocket::bind_ephemeral(),
    };
}

struct RtspRequest {
    std::string method;
    std::string uri;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string format_request(const RtspRequest& request) {
    std::ostringstream stream;
    stream << request.method << ' ' << request.uri << " RTSP/1.0\r\n";
    for (const auto& [name, value] : request.headers) {
        stream << name << ": " << value << "\r\n";
    }
    if (!request.body.empty()) {
        stream << "Content-Length: " << request.body.size() << "\r\n";
    }
    stream << "\r\n";
    stream << request.body;
    return stream.str();
}

uint16_t parse_u16(const std::string& value) {
    const auto parsed = std::strtoul(value.c_str(), nullptr, 10);
    if (parsed > 65535) {
        return 0;
    }
    return static_cast<uint16_t>(parsed);
}

std::map<std::string, std::string> parse_transport_parameters(const std::string& transport) {
    std::map<std::string, std::string> result;
    std::stringstream stream(transport);
    std::string token;

    while (std::getline(stream, token, ';')) {
        token = trim_ascii(token);
        if (token.empty()) {
            continue;
        }

        const auto separator = token.find('=');
        if (separator == std::string::npos) {
            result[lower_ascii(token)] = {};
        } else {
            result[lower_ascii(trim_ascii(token.substr(0, separator)))] = trim_ascii(token.substr(separator + 1));
        }
    }

    return result;
}

uint16_t transport_port(const std::map<std::string, std::string>& transport, const char* key) {
    const auto it = transport.find(key);
    return it == transport.end() ? 0 : parse_u16(it->second);
}

std::string session_id_from_header(std::string value) {
    const auto separator = value.find(';');
    if (separator != std::string::npos) {
        value.resize(separator);
    }
    return trim_ascii(std::move(value));
}

std::string make_stream_uri(const OutputDevice& output) {
    return "rtsp://" + output.endpoint_host + "/" + output.id;
}

double airplay_volume_db(int volume) {
    const int clamped = std::clamp(volume, 0, 100);
    if (clamped == 0) {
        return -144.0;
    }
    return std::max(-144.0, 20.0 * std::log10(static_cast<double>(clamped) / 100.0));
}

std::string make_volume_parameter_body(int volume) {
    std::ostringstream body;
    body << std::fixed << std::setprecision(6)
         << "volume: " << airplay_volume_db(volume) << "\r\n";
    return body.str();
}

std::string make_announce_sdp(const OutputDevice& output, const PcmFormat& format) {
    if (format.channels != 2 || format.bits_per_sample != 16) {
        throw std::invalid_argument("AirPlay SDP writer only supports stereo 16-bit PCM.");
    }

    const auto session_id = static_cast<unsigned long long>(std::time(nullptr));
    std::ostringstream sdp;
    sdp << "v=0\r\n";
    sdp << "o=FoobarUniversalMultiroom " << session_id << " 1 IN IP4 0.0.0.0\r\n";
    sdp << "s=Foobar Universal Multiroom\r\n";
    sdp << "c=IN IP4 " << (output.endpoint_host.empty() ? "0.0.0.0" : output.endpoint_host) << "\r\n";
    sdp << "t=0 0\r\n";
    sdp << "m=audio 0 RTP/AVP 96\r\n";
    sdp << "a=rtpmap:96 L16/" << format.sample_rate << "/" << format.channels << "\r\n";
    sdp << "a=fmtp:96 " << format.channels << " " << format.bits_per_sample << "\r\n";
    sdp << "a=control:streamid=0\r\n";
    sdp << "a=mode:record\r\n";
    return sdp.str();
}

size_t content_length_from(const std::map<std::string, std::string>& headers) {
    const auto it = headers.find("content-length");
    if (it == headers.end() || it->second.empty()) {
        return 0;
    }

    return static_cast<size_t>(std::strtoull(it->second.c_str(), nullptr, 10));
}

AirPlayRtspResponse parse_response(const std::string& raw) {
    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        throw std::runtime_error("RTSP response is missing header terminator.");
    }

    AirPlayRtspResponse response;
    std::istringstream stream(raw.substr(0, header_end));
    std::string line;

    if (!std::getline(stream, line)) {
        throw std::runtime_error("RTSP response is empty.");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream status(line);
    std::string protocol;
    status >> protocol >> response.status_code;
    std::getline(status, response.reason);
    response.reason = trim_ascii(response.reason);
    if ((protocol.rfind("RTSP/", 0) != 0 && protocol.rfind("HTTP/", 0) != 0) || response.status_code == 0) {
        throw std::runtime_error("RTSP response has an invalid status line.");
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }

        auto name = lower_ascii(trim_ascii(line.substr(0, separator)));
        auto value = trim_ascii(line.substr(separator + 1));
        if (!name.empty()) {
            response.headers[std::move(name)] = std::move(value);
        }
    }

    response.body = raw.substr(header_end + 4);
    return response;
}

std::string header_value_from_message(const std::string& message, const std::string& header_name) {
    const auto header_end = message.find("\r\n\r\n");
    const auto header_text = header_end == std::string::npos ? message : message.substr(0, header_end);
    std::istringstream stream(header_text);
    std::string line;
    const auto wanted = lower_ascii(header_name);

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }
        auto name = lower_ascii(trim_ascii(line.substr(0, separator)));
        if (name == wanted) {
            return trim_ascii(line.substr(separator + 1));
        }
    }

    return {};
}

}  // namespace

class AirPlayRtspControlClient::Connection {
    friend class AirPlayRtspControlClient;

public:
    Connection() = default;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    ~Connection() {
        close();
    }

    void connect_to(const std::string& host, uint16_t port) {
#ifdef _WIN32
        winsock_ = std::make_unique<WinsockSession>();
#endif
        remote_host_ = host;
        AddrInfoList addresses(host, port, SOCK_STREAM);

        const auto try_connect_family = [&](int family) {
            for (addrinfo* address = addresses.begin(); address != nullptr; address = address->ai_next) {
                if (family != AF_UNSPEC && address->ai_family != family) {
                    continue;
                }

                socket_handle_t candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
                if (candidate == kInvalidSocket) {
                    continue;
                }

                if (::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
                    handle_ = candidate;
                    configure_timeouts();
                    return true;
                }

                close_socket(candidate);
            }
            return false;
        };

        if (try_connect_family(AF_INET) || try_connect_family(AF_UNSPEC)) {
            return;
        }

        throw std::runtime_error("Could not connect to AirPlay endpoint: " + host + ":" + std::to_string(port));
    }

    AirPlayRtspResponse exchange(const RtspRequest& request) {
        const auto bytes = format_request(request);
        send_all(bytes);
        return read_response();
    }

    AirPlayRtspResponse request(
        const std::string& method,
        const std::string& uri,
        std::map<std::string, std::string> headers = {},
        std::string body = {}) {
        headers.emplace("CSeq", std::to_string(next_cseq_++));
        headers.emplace("User-Agent", "FoobarUniversalMultiroom/0.1");

        RtspRequest request;
        request.method = method;
        request.uri = uri;
        request.headers = std::move(headers);
        request.body = std::move(body);
        return exchange(request);
    }

    AirPlayRtspResponse request_success(
        const std::string& method,
        const std::string& uri,
        std::map<std::string, std::string> headers = {},
        std::string body = {}) {
        AirPlayRtspResponse response;
        try {
            response = request(method, uri, std::move(headers), std::move(body));
        } catch (const std::exception& e) {
            throw std::runtime_error("AirPlay RTSP " + method + " " + uri + " failed: " + e.what());
        }
        if (!response.successful()) {
            throw std::runtime_error(
                "AirPlay RTSP " + method + " failed with status " +
                std::to_string(response.status_code) + " " + response.reason);
        }
        return response;
    }

    void bind_transport_ports() {
        udp_ports_ = bind_local_udp_ports();
    }

    const LocalUdpPorts& local_udp_ports() const {
        return udp_ports_;
    }

    void set_remote_data_endpoint(uint16_t port) {
        remote_data_port_ = port;
    }

    void set_stream_uri(std::string stream_uri) {
        stream_uri_ = std::move(stream_uri);
    }

    void send_audio_packet(const ScheduledPacket& packet, const void* frames, size_t bytes) {
        if (frames == nullptr && bytes != 0) {
            throw std::invalid_argument("RTP audio frame buffer cannot be null when bytes are present.");
        }
        if (bytes == 0) {
            return;
        }

        if (!ap2_audio_key_.empty()) {
            ensure_airplay2_sync_started(packet.presentation_timestamp);
            maybe_send_airplay2_sync(false, packet.presentation_timestamp);

            const auto rtp_timestamp =
                static_cast<uint32_t>((packet.presentation_timestamp + kAirPlay2LatencyFrames) & 0xFFFFFFFFu);
            const auto rtp_sequence = next_rtp_sequence_++;
            auto header = make_rtp_header(
                kRtpPayloadTypeAirPlay2Realtime,
                ap2_first_audio_,
                rtp_sequence,
                rtp_timestamp,
                ap2_session_id_ == 0 ? kDefaultSsrc : ap2_session_id_);
            ap2_first_audio_ = false;

            const auto payload = ap2_audio_encoding_ == AirPlay2AudioEncoding::Pcm
                ? make_airplay2_pcm_payload(frames, bytes)
                : make_alac_uncompressed_frame(frames, bytes);
            const auto encrypted = encrypt_airplay2_audio_payload(ap2_audio_key_, ap2_audio_nonce_++, header, payload);
            header.insert(header.end(), encrypted.begin(), encrypted.end());
            udp_ports_.data.send_to(remote_host_, remote_data_port_, header);
            rtp_clock_initialized_ = true;
            next_rtp_timestamp_ = rtp_timestamp + static_cast<uint32_t>(kAirPlay2FramesPerPacket);
            return;
        }

        const auto timestamp = static_cast<uint32_t>(packet.presentation_timestamp & 0xFFFFFFFFu);
        const auto frame_count = static_cast<uint32_t>(bytes / (kAirPlay2Channels * sizeof(int16_t)));
        const auto rtp = make_rtp_l16_packet(next_rtp_sequence_++, timestamp, kDefaultSsrc, frames, bytes);
        udp_ports_.data.send_to(remote_host_, remote_data_port_, rtp);
        rtp_clock_initialized_ = true;
        next_rtp_timestamp_ = timestamp + frame_count;
    }

    void set_volume(const std::string& rtsp_session_id, int volume) {
        if (stream_uri_.empty()) {
            throw std::logic_error("Cannot set AirPlay volume before stream URI is known.");
        }

        request_success(
            "SET_PARAMETER",
            stream_uri_,
            {
                {"Content-Type", "text/parameters"},
                {"Session", rtsp_session_id},
            },
            make_volume_parameter_body(volume));
    }

    void flush(const std::string& rtsp_session_id) {
        if (stream_uri_.empty()) {
            throw std::logic_error("Cannot flush AirPlay stream before stream URI is known.");
        }

        auto headers = !ap2_audio_key_.empty()
            ? ap2_headers({
                {"Range", "npt=0-"},
                {"RTP-Info", make_rtp_info_header()},
                {"Session", "0"},
            })
            : std::map<std::string, std::string>{
                {"RTP-Info", make_rtp_info_header()},
                {"Session", rtsp_session_id},
            };

        static_cast<void>(request("FLUSH", stream_uri_, std::move(headers)));
    }

    AirPlayNegotiatedSession open_airplay2_transient(
        const OutputDevice& output,
        const PcmFormat& format,
        const std::optional<AirPlayPairingCredentials>& credentials) {
        const auto preferred_encoding = preferred_airplay2_audio_encoding(output);
        try {
            return open_airplay2_transient_with_encoding(output, format, credentials, preferred_encoding);
        } catch (const AirPlay2StreamSetupRejected& e) {
            if (e.status_code() != 400 || preferred_encoding == AirPlay2AudioEncoding::Pcm) {
                throw;
            }
            close();
            return open_airplay2_transient_with_encoding(output, format, credentials, AirPlay2AudioEncoding::Pcm);
        }
    }

    AirPlayNegotiatedSession open_airplay2_transient_with_encoding(
        const OutputDevice& output,
        const PcmFormat& format,
        const std::optional<AirPlayPairingCredentials>& credentials,
        AirPlay2AudioEncoding audio_encoding) {
        if (format.sample_rate != 44100 || format.channels != 2 || format.bits_per_sample != 16) {
            throw std::invalid_argument("AirPlay 2 MVP currently requires 44.1 kHz stereo 16-bit PCM.");
        }

        ap2_audio_encoding_ = audio_encoding;
        dacp_id_ = random_hex(8);
        sender_device_id_ = random_hex(6);
        active_remote_ = std::to_string(random_u32());
        ap2_session_id_ = random_u32();
        if (ap2_session_id_ == 0) {
            ap2_session_id_ = kDefaultSsrc;
        }
        ap2_session_uuid_ = random_uuid_text();

        connect_to(output.endpoint_host, output.endpoint_port);
        bind_transport_ports();
        start_timing_worker();

        static_cast<void>(request_success(
            "GET",
            "/info",
            ap2_headers()));

        AirPlay2Bytes shared_secret;
        AirPlay2EncryptedKeys keys;
        if (credentials) {
            const auto verified = run_pair_verify(*credentials);
            shared_secret = verified.shared_secret;
            keys = verified.keys;
        } else {
            shared_secret = run_transient_pair_setup();
            keys = derive_airplay2_encrypted_keys(shared_secret);
        }

        auto audio_key = shared_secret;
        if (audio_key.size() > 32) {
            audio_key.resize(32);
        }
        if (audio_key.size() != 32) {
            throw std::runtime_error("AirPlay 2 pairing did not produce a usable audio key.");
        }
        ap2_audio_key_ = std::move(audio_key);
        ap2_audio_nonce_ = 0;

        control_cipher_ = std::make_unique<AirPlay2FrameCipher>(keys.control_write, keys.control_read);
        event_cipher_ = std::make_unique<AirPlay2FrameCipher>(keys.event_read, keys.event_write);

        static_cast<void>(request_success(
            "GET",
            "/info",
            ap2_headers()));

        stream_uri_ = "rtsp://" + local_address_text() + "/" + std::to_string(ap2_session_id_);
        set_stream_uri(stream_uri_);

        AirPlayRtspResponse session_setup;
        try {
            const auto session_body = make_ap2_session_setup_body();
            session_setup = request_success(
                "SETUP",
                stream_uri_,
                ap2_setup_headers(),
                string_from_bytes(session_body));
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("AirPlay 2 session SETUP failed: ") + e.what());
        }

        const auto event_port = parse_ap2_event_port(session_setup.body);
        if (event_port != 0) {
            connect_event_channel(output.endpoint_host, event_port);
        }

        static_cast<void>(request_success(
            "RECORD",
            stream_uri_,
            ap2_headers()));

        AirPlayRtspResponse stream_setup;
        try {
            const auto stream_body = make_ap2_stream_setup_body(audio_encoding);
            stream_setup = request(
                "SETUP",
                stream_uri_,
                ap2_setup_headers(),
                string_from_bytes(stream_body));
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("AirPlay 2 stream SETUP failed: ") + e.what());
        }
        if (!stream_setup.successful()) {
            throw AirPlay2StreamSetupRejected(stream_setup.status_code, stream_setup.reason);
        }

        const auto stream_ports = parse_ap2_stream_ports(stream_setup.body);
        if (stream_ports.first == 0) {
            throw std::runtime_error("AirPlay 2 stream SETUP did not return a data port.");
        }
        remote_data_port_ = stream_ports.first;
        remote_control_port_ = stream_ports.second == 0 ? stream_ports.first : stream_ports.second;
        ap2_sync_started_ = false;

        auto ports = local_udp_ports().to_transport_ports();
        ports.server_data_port = stream_ports.first;
        ports.server_control_port = stream_ports.second == 0 ? stream_ports.first : stream_ports.second;
        ports.server_timing_port = stream_ports.first;

        AirPlayNegotiatedSession session;
        session.rtsp_session_id = std::to_string(ap2_session_id_);
        session.stream_uri = stream_uri_;
        session.server_name = "AirPlay2";
        session.supported_methods = {"GET", "SETUP", "RECORD", "SET_PARAMETER", "FLUSH", "TEARDOWN"};
        session.ports = ports;
        return session;
    }

    void close() {
        event_running_ = false;
        timing_running_ = false;
        const auto event_handle = event_handle_;
        close_socket(event_handle);
        if (event_thread_.joinable()) {
            event_thread_.join();
        }
        if (timing_thread_.joinable()) {
            timing_thread_.join();
        }
        event_handle_ = kInvalidSocket;
        close_socket(handle_);
        handle_ = kInvalidSocket;
        reset_session_state();
#ifdef _WIN32
        winsock_.reset();
#endif
    }

private:
    void reset_session_state() {
        next_cseq_ = 1;
        next_rtp_sequence_ = 0;
        next_rtp_timestamp_ = 0;
        rtp_clock_initialized_ = false;
        remote_data_port_ = 0;
        remote_control_port_ = 0;
        ap2_session_id_ = 0;
        ap2_session_uuid_.clear();
        ap2_audio_nonce_ = 0;
        ap2_sync_start_rtp_ = 0;
        ap2_audio_encoding_ = AirPlay2AudioEncoding::Alac;
        ap2_first_audio_ = true;
        ap2_sync_started_ = false;
        ap2_last_sync_at_ = std::chrono::steady_clock::time_point::min();
        remote_host_.clear();
        stream_uri_.clear();
        dacp_id_.clear();
        sender_device_id_.clear();
        active_remote_.clear();
        ap2_audio_key_.clear();
        control_cipher_.reset();
        event_cipher_.reset();
        udp_ports_ = LocalUdpPorts{};
    }

    std::map<std::string, std::string> ap2_headers(std::map<std::string, std::string> headers = {}) const {
        headers.emplace("User-Agent", "AirPlay/550.10");
        headers.emplace("DACP-ID", dacp_id_);
        headers.emplace("Active-Remote", active_remote_);
        headers.emplace("Client-Instance", dacp_id_);
        return headers;
    }

    std::map<std::string, std::string> ap2_setup_headers() const {
        return ap2_headers({
            {"Content-Type", "application/x-apple-binary-plist"},
        });
    }

    std::string make_rtp_info_header() const {
        const auto timestamp = rtp_clock_initialized_ ? next_rtp_timestamp_ : uint32_t{};
        return "seq=" + std::to_string(next_rtp_sequence_) + ";rtptime=" + std::to_string(timestamp);
    }

    AirPlayRtspResponse http_post_success(
        const std::string& uri,
        const std::string& content_type,
        const AirPlay2Bytes& body,
        const char* hkp = "4") {
        std::ostringstream request;
        request << "POST " << uri << " HTTP/1.1\r\n";
        request << "CSeq: " << next_cseq_++ << "\r\n";
        request << "User-Agent: AirPlay/550.10\r\n";
        request << "Connection: keep-alive\r\n";
        request << "X-Apple-HKP: " << hkp << "\r\n";
        request << "DACP-ID: " << dacp_id_ << "\r\n";
        request << "Active-Remote: " << active_remote_ << "\r\n";
        request << "Client-Instance: " << dacp_id_ << "\r\n";
        request << "X-Apple-Client-Name: FoobarUniversalMultiroom\r\n";
        request << "Content-Type: " << content_type << "\r\n";
        request << "Content-Length: " << body.size() << "\r\n\r\n";
        request << string_from_bytes(body);

        AirPlayRtspResponse response;
        try {
            send_all(request.str());
            response = read_response();
        } catch (const std::exception& e) {
            throw std::runtime_error("AirPlay 2 HTTP " + uri + " failed: " + e.what());
        }
        if (!response.successful()) {
            if (response.status_code == 470 || response.status_code == 403) {
                throw std::runtime_error("AirPlay 2 device requires explicit PIN pairing before streaming.");
            }
            throw std::runtime_error(
                "AirPlay 2 HTTP " + uri + " failed with status " +
                std::to_string(response.status_code) + " " + response.reason);
        }
        return response;
    }

    AirPlayPairingCredentials pair_with_pin(const OutputDevice& output, const std::string& pin) {
        if (pin.empty()) {
            throw std::invalid_argument("AirPlay PIN cannot be empty.");
        }

        dacp_id_ = random_hex(8);
        active_remote_ = std::to_string(random_u32());
        connect_to(output.endpoint_host, output.endpoint_port);

        static_cast<void>(http_post_success("/pair-pin-start", "application/octet-stream", {}, "3"));

        const auto m1 = fxchain::airplay::tlv::encode({
            {fxchain::airplay::tlv::Method, {0x00}},
            {fxchain::airplay::tlv::State, {0x01}},
        });
        const auto m2_response = http_post_success("/pair-setup", "application/octet-stream", m1, "3");
        const auto m2 = fxchain::airplay::tlv::decode(bytes_from_string(m2_response.body));
        if (auto error = fxchain::airplay::tlv::get(m2, fxchain::airplay::tlv::Error)) {
            throw std::runtime_error("AirPlay PIN pair-setup M2 returned error " +
                                     std::to_string(error->empty() ? 0 : error->front()) + ".");
        }

        const auto salt = fxchain::airplay::tlv::get(m2, fxchain::airplay::tlv::Salt);
        const auto server_public_key = fxchain::airplay::tlv::get(m2, fxchain::airplay::tlv::PublicKey);
        if (!salt || !server_public_key) {
            throw std::runtime_error("AirPlay PIN pair-setup M2 was incomplete.");
        }

        fxchain::airplay::SrpClient srp;
        srp.start(pin);
        if (!srp.process(*salt, *server_public_key)) {
            throw std::runtime_error("AirPlay PIN pairing rejected receiver parameters.");
        }

        const auto m3 = fxchain::airplay::tlv::encode({
            {fxchain::airplay::tlv::State, {0x03}},
            {fxchain::airplay::tlv::PublicKey, srp.publicA()},
            {fxchain::airplay::tlv::Proof, srp.proofM1()},
        });
        const auto m4_response = http_post_success("/pair-setup", "application/octet-stream", m3, "3");
        const auto m4 = fxchain::airplay::tlv::decode(bytes_from_string(m4_response.body));
        if (auto error = fxchain::airplay::tlv::get(m4, fxchain::airplay::tlv::Error)) {
            throw std::runtime_error("AirPlay PIN was not accepted; pair-setup M4 returned error " +
                                     std::to_string(error->empty() ? 0 : error->front()) + ".");
        }
        if (auto proof = fxchain::airplay::tlv::get(m4, fxchain::airplay::tlv::Proof)) {
            if (!srp.verifyServerProof(*proof)) {
                throw std::runtime_error("AirPlay PIN pair-setup server proof verification failed.");
            }
        }

        AirPlayPairingCredentials credentials;
        credentials.output_id = output.id;
        credentials.client_id = random_uuid_text();
        credentials.controller_seed = fxchain::airplay::randomBytes(32);
        const auto controller_public_key = fxchain::airplay::ed25519PublicFromSeed(credentials.controller_seed);

        const auto session_key = hkdf_sha512("Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", srp.sessionKey());
        const auto controller_sign_key =
            hkdf_sha512("Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", srp.sessionKey());

        auto device_info = controller_sign_key;
        const auto client_id_bytes = bytes_from_string(credentials.client_id);
        device_info.insert(device_info.end(), client_id_bytes.begin(), client_id_bytes.end());
        device_info.insert(device_info.end(), controller_public_key.begin(), controller_public_key.end());
        const auto signature = fxchain::airplay::ed25519Sign(credentials.controller_seed, device_info);

        const auto inner = fxchain::airplay::tlv::encode({
            {fxchain::airplay::tlv::Identifier, client_id_bytes},
            {fxchain::airplay::tlv::PublicKey, controller_public_key},
            {fxchain::airplay::tlv::Signature, signature},
        });
        const auto encrypted = encrypt_with_named_nonce(session_key, "PS-Msg05", inner);
        const auto m5 = fxchain::airplay::tlv::encode({
            {fxchain::airplay::tlv::State, {0x05}},
            {fxchain::airplay::tlv::EncryptedData, encrypted},
        });
        const auto m6_response = http_post_success("/pair-setup", "application/octet-stream", m5, "3");
        const auto m6 = fxchain::airplay::tlv::decode(bytes_from_string(m6_response.body));
        if (auto error = fxchain::airplay::tlv::get(m6, fxchain::airplay::tlv::Error)) {
            throw std::runtime_error("AirPlay PIN pair-setup M6 returned error " +
                                     std::to_string(error->empty() ? 0 : error->front()) + ".");
        }

        const auto encrypted_m6 = fxchain::airplay::tlv::get(m6, fxchain::airplay::tlv::EncryptedData);
        if (!encrypted_m6) {
            throw std::runtime_error("AirPlay PIN pair-setup M6 was incomplete.");
        }
        const auto decrypted_m6 = decrypt_with_named_nonce(session_key, "PS-Msg06", *encrypted_m6);
        const auto m6_inner = fxchain::airplay::tlv::decode(decrypted_m6);
        const auto accessory_id = fxchain::airplay::tlv::get(m6_inner, fxchain::airplay::tlv::Identifier);
        const auto accessory_public_key = fxchain::airplay::tlv::get(m6_inner, fxchain::airplay::tlv::PublicKey);
        if (!accessory_id || !accessory_public_key) {
            throw std::runtime_error("AirPlay PIN pair-setup M6 did not return receiver credentials.");
        }
        credentials.accessory_identifier = *accessory_id;
        credentials.accessory_public_key = *accessory_public_key;
        return credentials;
    }

    AirPlay2PairVerifyM2 run_pair_verify(const AirPlayPairingCredentials& credentials) {
        if (!credentials.valid()) {
            throw std::runtime_error("Stored AirPlay pairing credentials are incomplete.");
        }

        AirPlay2PairVerifySession verify(credentials.client_id, credentials.controller_seed);
        const auto m1_response = http_post_success("/pair-verify", "application/octet-stream", verify.make_m1(), "3");
        const auto m2 = verify.handle_m2(bytes_from_string(m1_response.body), credentials.accessory_public_key);
        const auto m3_response = http_post_success("/pair-verify", "application/octet-stream", verify.make_m3(), "3");
        if (!m3_response.successful()) {
            throw std::runtime_error("AirPlay pair-verify M3 was rejected.");
        }
        return m2;
    }

    AirPlay2Bytes run_transient_pair_setup() {
        using namespace fxchain::airplay;

        static_cast<void>(http_post_success("/pair-pin-start", "application/octet-stream", {}));

        const auto m1 = tlv::encode({
            {tlv::Method, {0x00}},
            {tlv::State, {0x01}},
            {tlv::Flags, {0x10}},
        });
        const auto m2_response = http_post_success("/pair-setup", "application/octet-stream", m1);
        const auto m2 = tlv::decode(bytes_from_string(m2_response.body));
        if (auto error = tlv::get(m2, tlv::Error)) {
            throw std::runtime_error("AirPlay 2 transient pair-setup M2 returned error " +
                                     std::to_string(error->empty() ? 0 : error->front()) + ".");
        }

        const auto salt = tlv::get(m2, tlv::Salt);
        const auto server_public_key = tlv::get(m2, tlv::PublicKey);
        if (!salt || !server_public_key) {
            throw std::runtime_error("AirPlay 2 transient pair-setup M2 was incomplete.");
        }

        SrpClient srp;
        srp.start("3939");
        if (!srp.process(*salt, *server_public_key)) {
            throw std::runtime_error("AirPlay 2 SRP rejected receiver parameters.");
        }

        const auto m3 = tlv::encode({
            {tlv::State, {0x03}},
            {tlv::PublicKey, srp.publicA()},
            {tlv::Proof, srp.proofM1()},
        });
        const auto m4_response = http_post_success("/pair-setup", "application/octet-stream", m3);
        const auto m4 = tlv::decode(bytes_from_string(m4_response.body));
        if (auto error = tlv::get(m4, tlv::Error)) {
            throw std::runtime_error("AirPlay 2 transient pair-setup M4 returned error " +
                                     std::to_string(error->empty() ? 0 : error->front()) + ".");
        }
        if (auto proof = tlv::get(m4, tlv::Proof)) {
            if (!srp.verifyServerProof(*proof)) {
                throw std::runtime_error("AirPlay 2 SRP server proof verification failed.");
            }
        }

        return srp.sessionKey();
    }

    AirPlay2Bytes make_ap2_session_setup_body() const {
        using namespace fxchain::airplay::bplist;

        const auto device_id = colon_hex_text(sender_device_id_);
        Dict dict;
        dict.emplace_back("deviceID", Value::str(device_id));
        dict.emplace_back("sessionUUID", Value::str(ap2_session_uuid_));
        // Keep session timing aligned with the NTP timing worker and 0xD4 sync packets below.
        dict.emplace_back("timingPort", Value::integer(local_udp_ports().timing.port()));
        dict.emplace_back("timingProtocol", Value::str("NTP"));
        dict.emplace_back("isMultiSelectAirPlay", Value::boolean(true));
        dict.emplace_back("groupContainsGroupLeader", Value::boolean(false));
        dict.emplace_back("macAddress", Value::str(device_id));
        dict.emplace_back("model", Value::str("iPhone14,3"));
        dict.emplace_back("name", Value::str("FoobarUniversalMultiroom"));
        dict.emplace_back("osBuildVersion", Value::str("20F66"));
        dict.emplace_back("osName", Value::str("iPhone OS"));
        dict.emplace_back("osVersion", Value::str("16.5"));
        dict.emplace_back("senderSupportsRelay", Value::boolean(false));
        dict.emplace_back("sourceVersion", Value::str("690.7.1"));
        dict.emplace_back("statsCollectionEnabled", Value::boolean(false));
        return fxchain::airplay::bplist::encode(Value::object(std::move(dict)));
    }

    AirPlay2Bytes make_ap2_stream_setup_body(AirPlay2AudioEncoding audio_encoding) const {
        using namespace fxchain::airplay::bplist;

        Dict stream;
        stream.emplace_back(
            "audioFormat",
            Value::integer(audio_encoding == AirPlay2AudioEncoding::Pcm ? 0x800 : 0x40000));
        stream.emplace_back("audioMode", Value::str("default"));
        stream.emplace_back("controlPort", Value::integer(local_udp_ports().control.port()));
        stream.emplace_back("ct", Value::integer(audio_encoding == AirPlay2AudioEncoding::Pcm ? 1 : 2));
        stream.emplace_back("isMedia", Value::boolean(true));
        stream.emplace_back("latencyMax", Value::integer(kAirPlay2LatencyFrames));
        stream.emplace_back("latencyMin", Value::integer(kAirPlay2MinimumLatencyFrames));
        stream.emplace_back("shk", Value::bytes(ap2_audio_key_));
        stream.emplace_back("spf", Value::integer(static_cast<int64_t>(kAirPlay2FramesPerPacket)));
        stream.emplace_back("sr", Value::integer(44100));
        stream.emplace_back("type", Value::integer(0x60));
        stream.emplace_back("supportsDynamicStreamID", Value::boolean(false));
        stream.emplace_back("streamConnectionID", Value::integer(ap2_session_id_));

        Array streams;
        streams.push_back(Value::object(std::move(stream)));

        Dict dict;
        dict.emplace_back("streams", Value::array(std::move(streams)));
        return fxchain::airplay::bplist::encode(Value::object(std::move(dict)));
    }

    uint16_t parse_ap2_event_port(const std::string& body) const {
        const auto decoded = fxchain::airplay::bplist::decode(bytes_from_string(body));
        if (!decoded) {
            return 0;
        }
        const auto* event_port = decoded->find("eventPort");
        return event_port == nullptr ? 0 : static_cast<uint16_t>(event_port->asInt());
    }

    std::pair<uint16_t, uint16_t> parse_ap2_stream_ports(const std::string& body) const {
        const auto decoded = fxchain::airplay::bplist::decode(bytes_from_string(body));
        if (!decoded) {
            return {};
        }
        const auto* streams = decoded->find("streams");
        if (streams == nullptr || streams->type != fxchain::airplay::bplist::Value::Type::Arr || streams->arr.empty()) {
            return {};
        }

        const auto& first = streams->arr.front();
        const auto* data_port = first.find("dataPort");
        const auto* control_port = first.find("controlPort");
        return {
            data_port == nullptr ? uint16_t{} : static_cast<uint16_t>(data_port->asInt()),
            control_port == nullptr ? uint16_t{} : static_cast<uint16_t>(control_port->asInt()),
        };
    }

    void ensure_airplay2_sync_started(uint64_t presentation_timestamp) {
        if (ap2_sync_started_) {
            return;
        }

        const auto now = ntp_now();
        const auto now_rtp = ntp_to_rtp_timestamp(now, kAirPlay2SampleRate);
        ap2_sync_start_rtp_ = now_rtp > presentation_timestamp ? now_rtp - presentation_timestamp : 0;
        ap2_sync_started_ = true;
        send_airplay2_sync_packet(true, presentation_timestamp);
    }

    void maybe_send_airplay2_sync(bool first, uint64_t presentation_timestamp) {
        if (first || std::chrono::steady_clock::now() - ap2_last_sync_at_ >= std::chrono::seconds(1)) {
            send_airplay2_sync_packet(first, presentation_timestamp);
        }
    }

    void send_airplay2_sync_packet(bool first, uint64_t presentation_timestamp) {
        if (remote_control_port_ == 0) {
            return;
        }

        const auto rtp_timestamp =
            static_cast<uint32_t>((presentation_timestamp + kAirPlay2LatencyFrames) & 0xFFFFFFFFu);
        const auto ntp = rtp_timestamp_to_ntp(ap2_sync_start_rtp_ + presentation_timestamp, kAirPlay2SampleRate);

        std::vector<uint8_t> sync;
        sync.reserve(20);
        sync.push_back(first ? 0x90 : 0x80);
        sync.push_back(0xD4);
        write_u16_be(sync, 0x0007);
        write_u32_be(sync, rtp_timestamp - kAirPlay2LatencyFrames);
        write_u32_be(sync, static_cast<uint32_t>(ntp >> 32));
        write_u32_be(sync, static_cast<uint32_t>(ntp & 0xFFFFFFFFu));
        write_u32_be(sync, rtp_timestamp);
        udp_ports_.control.send_to(remote_host_, remote_control_port_, sync);
        ap2_last_sync_at_ = std::chrono::steady_clock::now();
    }

    std::string local_address_text() const {
        sockaddr_storage address = {};
#ifdef _WIN32
        int address_size = sizeof(address);
#else
        socklen_t address_size = sizeof(address);
#endif
        if (getsockname(handle_, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
            return "0.0.0.0";
        }

        char host[NI_MAXHOST] = {};
        if (getnameinfo(
                reinterpret_cast<const sockaddr*>(&address),
                address_size,
                host,
                sizeof(host),
                nullptr,
                0,
                NI_NUMERICHOST) != 0) {
            return "0.0.0.0";
        }
        return host;
    }

    void connect_event_channel(const std::string& host, uint16_t port) {
        AddrInfoList addresses(host, port, SOCK_STREAM);
        for (addrinfo* address = addresses.begin(); address != nullptr; address = address->ai_next) {
            socket_handle_t candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (candidate == kInvalidSocket) {
                continue;
            }

            if (::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
                event_handle_ = candidate;
                configure_socket_timeouts(event_handle_, 1000);
                start_event_worker();
                return;
            }

            close_socket(candidate);
        }
    }

    void configure_timeouts() {
        if (handle_ == kInvalidSocket) {
            return;
        }

        configure_socket_timeouts(handle_, 10000);
    }

    void configure_socket_timeouts(socket_handle_t handle, unsigned long timeout_ms) {
        if (handle == kInvalidSocket) {
            return;
        }

#ifdef _WIN32
        DWORD timeout = static_cast<DWORD>(timeout_ms);
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout = {};
        timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
        timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    }

    void start_event_worker() {
        if (!event_cipher_ || event_handle_ == kInvalidSocket || event_thread_.joinable()) {
            return;
        }

        const auto event_handle = event_handle_;
        event_running_ = true;
        event_thread_ = std::thread([this, event_handle] {
            event_loop(event_handle);
        });
    }

    void event_loop(socket_handle_t event_handle) {
        std::string plain_buffer;
        while (event_running_) {
            char buffer[2048] = {};
#ifdef _WIN32
            const int received = recv(event_handle, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
            const ssize_t received = recv(event_handle, buffer, sizeof(buffer), 0);
#endif
            if (received <= 0) {
                continue;
            }

            try {
                const AirPlay2Bytes encrypted(
                    reinterpret_cast<const uint8_t*>(buffer),
                    reinterpret_cast<const uint8_t*>(buffer) + received);
                auto frames = event_cipher_->decrypt_available(encrypted);
                for (const auto& frame : frames) {
                    plain_buffer.append(frame.begin(), frame.end());
                    respond_to_event_messages(event_handle, plain_buffer);
                }
            } catch (const std::exception&) {
                plain_buffer.clear();
            }
        }
    }

    void start_timing_worker() {
        if (timing_thread_.joinable()) {
            return;
        }

        udp_ports_.timing.set_receive_timeout(500);
        timing_running_ = true;
        timing_thread_ = std::thread([this] {
            timing_loop();
        });
    }

    void timing_loop() {
        while (timing_running_) {
            std::vector<uint8_t> request;
            sockaddr_storage from = {};
            int from_size = 0;
            if (!udp_ports_.timing.receive_from(request, from, from_size) || request.size() < 32) {
                continue;
            }

            const auto now = ntp_now();
            std::vector<uint8_t> response;
            response.reserve(32);
            response.push_back(request[0]);
            response.push_back(0xD3);
            write_u16_be(response, 0x0007);
            write_u32_be(response, 0);
            response.insert(response.end(), request.begin() + 24, request.begin() + 32);
            write_u32_be(response, static_cast<uint32_t>(now >> 32));
            write_u32_be(response, static_cast<uint32_t>(now & 0xFFFFFFFFu));
            write_u32_be(response, static_cast<uint32_t>(now >> 32));
            write_u32_be(response, static_cast<uint32_t>(now & 0xFFFFFFFFu));
            udp_ports_.timing.send_to_address(from, from_size, response);
        }
    }

    void respond_to_event_messages(socket_handle_t event_handle, std::string& plain_buffer) {
        for (;;) {
            const auto header_end = plain_buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                return;
            }

            const auto headers = parse_response_like_request_headers(plain_buffer.substr(0, header_end + 4));
            const auto total_size = header_end + 4 + content_length_from(headers);
            if (plain_buffer.size() < total_size) {
                return;
            }

            const auto message = plain_buffer.substr(0, total_size);
            plain_buffer.erase(0, total_size);

            auto cseq = header_value_from_message(message, "CSeq");
            if (cseq.empty()) {
                cseq = "1";
            }
            const auto response = "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\n\r\n";
            send_event_response(event_handle, response);
        }
    }

    std::map<std::string, std::string> parse_response_like_request_headers(const std::string& header_block) const {
        std::map<std::string, std::string> headers;
        std::istringstream stream(header_block);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto separator = line.find(':');
            if (separator == std::string::npos) {
                continue;
            }
            headers[lower_ascii(trim_ascii(line.substr(0, separator)))] = trim_ascii(line.substr(separator + 1));
        }
        return headers;
    }

    void send_event_response(socket_handle_t event_handle, const std::string& response) {
        if (!event_cipher_ || event_handle == kInvalidSocket) {
            return;
        }
        const AirPlay2Bytes plaintext(response.begin(), response.end());
        const auto encrypted = event_cipher_->encrypt_frame(plaintext);
        size_t sent = 0;
        while (sent < encrypted.size()) {
            const auto remaining = encrypted.size() - sent;
#ifdef _WIN32
            const int rc = send(
                event_handle,
                reinterpret_cast<const char*>(encrypted.data() + sent),
                static_cast<int>(remaining),
                0);
#else
            const ssize_t rc = send(event_handle, encrypted.data() + sent, remaining, 0);
#endif
            if (rc <= 0) {
                return;
            }
            sent += static_cast<size_t>(rc);
        }
    }

    void send_all_raw(const uint8_t* data, size_t size) {
        size_t sent = 0;
        while (sent < size) {
            const auto remaining = size - sent;
#ifdef _WIN32
            const int rc = send(handle_, reinterpret_cast<const char*>(data + sent), static_cast<int>(remaining), 0);
#else
            const ssize_t rc = send(handle_, data + sent, remaining, 0);
#endif
            if (rc <= 0) {
                throw std::runtime_error("Could not write RTSP request to AirPlay endpoint.");
            }
            sent += static_cast<size_t>(rc);
        }
    }

    void send_all(const std::string& bytes) {
        if (!control_cipher_) {
            send_all_raw(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
            return;
        }

        const AirPlay2Bytes plaintext(bytes.begin(), bytes.end());
        const auto encrypted = control_cipher_->encrypt_frame(plaintext);
        send_all_raw(encrypted.data(), encrypted.size());
    }

    std::string read_plaintext_chunk() {
        char buffer[4096] = {};
#ifdef _WIN32
        const int received = recv(handle_, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        const ssize_t received = recv(handle_, buffer, sizeof(buffer), 0);
#endif
        if (received <= 0) {
            throw std::runtime_error("Could not read RTSP response from AirPlay endpoint.");
        }

        if (!control_cipher_) {
            return std::string(buffer, static_cast<size_t>(received));
        }

        const AirPlay2Bytes encrypted(
            reinterpret_cast<const uint8_t*>(buffer),
            reinterpret_cast<const uint8_t*>(buffer) + received);
        auto frames = control_cipher_->decrypt_available(encrypted);
        std::string plaintext;
        for (const auto& frame : frames) {
            plaintext.append(frame.begin(), frame.end());
        }
        return plaintext;
    }

    AirPlayRtspResponse read_response() {
        std::string raw;
        size_t header_end = std::string::npos;
        size_t expected_size = 0;

        for (;;) {
            raw += read_plaintext_chunk();
            if (header_end == std::string::npos) {
                header_end = raw.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const auto response = parse_response(raw.substr(0, header_end + 4));
                    expected_size = header_end + 4 + content_length_from(response.headers);
                }
            }

            if (header_end != std::string::npos && raw.size() >= expected_size) {
                return parse_response(raw.substr(0, expected_size));
            }
        }
    }

    socket_handle_t handle_ = kInvalidSocket;
    int next_cseq_ = 1;
    uint16_t next_rtp_sequence_ = 0;
    uint32_t next_rtp_timestamp_ = 0;
    bool rtp_clock_initialized_ = false;
    uint16_t remote_data_port_ = 0;
    uint16_t remote_control_port_ = 0;
    uint32_t ap2_session_id_ = 0;
    std::string ap2_session_uuid_;
    uint64_t ap2_audio_nonce_ = 0;
    uint64_t ap2_sync_start_rtp_ = 0;
    AirPlay2AudioEncoding ap2_audio_encoding_ = AirPlay2AudioEncoding::Alac;
    bool ap2_first_audio_ = true;
    bool ap2_sync_started_ = false;
    std::chrono::steady_clock::time_point ap2_last_sync_at_ = std::chrono::steady_clock::time_point::min();
    std::string remote_host_;
    std::string stream_uri_;
    std::string dacp_id_;
    std::string sender_device_id_;
    std::string active_remote_;
    AirPlay2Bytes ap2_audio_key_;
    std::unique_ptr<AirPlay2FrameCipher> control_cipher_;
    std::unique_ptr<AirPlay2FrameCipher> event_cipher_;
    std::atomic_bool event_running_ = false;
    std::thread event_thread_;
    socket_handle_t event_handle_ = kInvalidSocket;
    std::atomic_bool timing_running_ = false;
    std::thread timing_thread_;
    LocalUdpPorts udp_ports_;
#ifdef _WIN32
    std::unique_ptr<WinsockSession> winsock_;
#endif
};

bool AirPlayRtspResponse::successful() const {
    return status_code >= 200 && status_code < 300;
}

std::string AirPlayRtspResponse::header(const std::string& name) const {
    const auto it = headers.find(lower_ascii(name));
    return it == headers.end() ? std::string{} : it->second;
}

bool AirPlayPairingCredentials::valid() const {
    return !output_id.empty() &&
           !client_id.empty() &&
           controller_seed.size() == 32 &&
           !accessory_identifier.empty() &&
           accessory_public_key.size() == 32;
}

AirPlayRtspControlClient::AirPlayRtspControlClient(std::shared_ptr<AirPlayPairingStore> pairing_store)
    : pairing_store_(std::move(pairing_store)) {}

AirPlayRtspControlClient::~AirPlayRtspControlClient() = default;

std::optional<AirPlayPairingCredentials> AirPlayRtspControlClient::load_pairing_credentials(
    const std::string& output_id) {
    if (pairing_store_) {
        auto stored = pairing_store_->load(output_id);
        if (stored && stored->valid()) {
            memory_pairing_credentials_[output_id] = *stored;
            return stored;
        }
    }

    const auto it = memory_pairing_credentials_.find(output_id);
    if (it != memory_pairing_credentials_.end() && it->second.valid()) {
        return it->second;
    }
    return std::nullopt;
}

void AirPlayRtspControlClient::save_pairing_credentials(const AirPlayPairingCredentials& credentials) {
    if (!credentials.valid()) {
        throw std::invalid_argument("Cannot save incomplete AirPlay pairing credentials.");
    }

    memory_pairing_credentials_[credentials.output_id] = credentials;
    if (pairing_store_) {
        pairing_store_->save(credentials);
    }
}

AirPlayPairingResult AirPlayRtspControlClient::pair(const OutputDevice& output, const std::string& pin) {
    auto connection = std::make_unique<Connection>();
    auto credentials = connection->pair_with_pin(output, pin);
    save_pairing_credentials(credentials);

    AirPlayPairingResult result;
    result.credentials = std::move(credentials);
    result.stored = pairing_store_ != nullptr;
    return result;
}

AirPlayNegotiatedSession AirPlayRtspControlClient::open(const OutputDevice& output, const PcmFormat& format) {
    if (!output.supports_airplay2) {
        throw std::runtime_error("AirPlay 2 is required for streaming: " + output.id);
    }

    auto connection = std::make_unique<Connection>();
    auto credentials = load_pairing_credentials(output.id);
    auto session = connection->open_airplay2_transient(output, format, credentials);
    connections_[output.id] = std::move(connection);
    return session;
}

void AirPlayRtspControlClient::send_audio(
    const std::string& output_id,
    const std::string& rtsp_session_id,
    const ScheduledPacket& packet,
    const void* frames,
    size_t bytes) {
    static_cast<void>(rtsp_session_id);

    const auto it = connections_.find(output_id);
    if (it == connections_.end()) {
        throw std::logic_error("Cannot send AirPlay RTP audio for a closed session.");
    }

    it->second->send_audio_packet(packet, frames, bytes);
}

void AirPlayRtspControlClient::set_volume(
    const std::string& output_id,
    const std::string& rtsp_session_id,
    int volume) {
    const auto it = connections_.find(output_id);
    if (it == connections_.end()) {
        throw std::logic_error("Cannot set AirPlay volume for a closed session.");
    }
    if (rtsp_session_id.empty()) {
        throw std::invalid_argument("Cannot set AirPlay volume without an RTSP session id.");
    }

    it->second->set_volume(rtsp_session_id, volume);
}

void AirPlayRtspControlClient::flush(const std::string& output_id, const std::string& rtsp_session_id) {
    const auto it = connections_.find(output_id);
    if (it == connections_.end()) {
        return;
    }
    if (rtsp_session_id.empty()) {
        throw std::invalid_argument("Cannot flush AirPlay stream without an RTSP session id.");
    }

    it->second->flush(rtsp_session_id);
}

void AirPlayRtspControlClient::close(const std::string& output_id, const std::string& rtsp_session_id) {
    const auto it = connections_.find(output_id);
    if (it != connections_.end()) {
        if (!rtsp_session_id.empty()) {
            try {
                static_cast<void>(it->second->request(
                    "TEARDOWN",
                    "*",
                    {
                        {"Session", rtsp_session_id},
                    }));
            } catch (const std::exception&) {
                // The receiver may have already closed the control socket.
            }
        }
        connections_.erase(it);
    }
}

AirPlayPairingResult AirPlayLoopbackControlClient::pair(const OutputDevice& output, const std::string& pin) {
    static_cast<void>(pin);

    AirPlayPairingResult result;
    result.credentials.output_id = output.id;
    result.credentials.client_id = "loopback-client";
    result.credentials.controller_seed.assign(32, 0x11);
    result.credentials.accessory_identifier = bytes_from_string("loopback-accessory");
    result.credentials.accessory_public_key.assign(32, 0x22);
    result.stored = false;
    return result;
}

AirPlayNegotiatedSession AirPlayLoopbackControlClient::open(const OutputDevice& output, const PcmFormat& format) {
    static_cast<void>(format);

    ++open_count_;
    AirPlayNegotiatedSession session;
    session.rtsp_session_id = "loopback-" + output.id;
    session.stream_uri = make_stream_uri(output);
    session.server_name = "loopback";
    session.supported_methods = {"OPTIONS", "ANNOUNCE", "SETUP", "RECORD", "SET_PARAMETER", "FLUSH", "TEARDOWN"};
    session.ports.local_data_port = 6000;
    session.ports.local_control_port = 6001;
    session.ports.local_timing_port = 6002;
    session.ports.server_data_port = output.endpoint_port;
    session.ports.server_control_port = static_cast<uint16_t>(output.endpoint_port + 1);
    session.ports.server_timing_port = static_cast<uint16_t>(output.endpoint_port + 2);
    return session;
}

void AirPlayLoopbackControlClient::send_audio(
    const std::string& output_id,
    const std::string& rtsp_session_id,
    const ScheduledPacket& packet,
    const void* frames,
    size_t bytes) {
    static_cast<void>(output_id);
    static_cast<void>(rtsp_session_id);
    static_cast<void>(packet);
    if (frames == nullptr && bytes != 0) {
        throw std::invalid_argument("Loopback audio frame buffer cannot be null when bytes are present.");
    }
    if (bytes != 0) {
        ++audio_packet_count_;
    }
}

void AirPlayLoopbackControlClient::set_volume(
    const std::string& output_id,
    const std::string& rtsp_session_id,
    int volume) {
    static_cast<void>(output_id);
    static_cast<void>(rtsp_session_id);
    static_cast<void>(volume);
    ++volume_set_count_;
}

void AirPlayLoopbackControlClient::flush(const std::string& output_id, const std::string& rtsp_session_id) {
    static_cast<void>(output_id);
    static_cast<void>(rtsp_session_id);
    ++flush_count_;
}

void AirPlayLoopbackControlClient::close(const std::string& output_id, const std::string& rtsp_session_id) {
    static_cast<void>(output_id);
    static_cast<void>(rtsp_session_id);
    ++close_count_;
}

size_t AirPlayLoopbackControlClient::open_count() const {
    return open_count_;
}

size_t AirPlayLoopbackControlClient::audio_packet_count() const {
    return audio_packet_count_;
}

size_t AirPlayLoopbackControlClient::volume_set_count() const {
    return volume_set_count_;
}

size_t AirPlayLoopbackControlClient::flush_count() const {
    return flush_count_;
}

size_t AirPlayLoopbackControlClient::close_count() const {
    return close_count_;
}

std::shared_ptr<AirPlayControlClient> make_airplay_rtsp_control_client(
    std::shared_ptr<AirPlayPairingStore> pairing_store) {
    return std::make_shared<AirPlayRtspControlClient>(std::move(pairing_store));
}

std::shared_ptr<AirPlayControlClient> make_airplay_loopback_control_client() {
    return std::make_shared<AirPlayLoopbackControlClient>();
}

}  // namespace multiroom::airplay
