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
constexpr uint32_t kDefaultSsrc = 0x46424d52;  // FBMR

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

std::string random_mac_text() {
    const auto bytes = fxchain::airplay::randomBytes(6);
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) {
            stream << ':';
        }
        stream << std::setw(2) << static_cast<int>(bytes[index]);
    }
    return stream.str();
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
public:
    explicit Connection(std::shared_ptr<AirPlay2CredentialStore> credential_store)
        : credential_store_(std::move(credential_store)) {}

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

        for (addrinfo* address = addresses.begin(); address != nullptr; address = address->ai_next) {
            socket_handle_t candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (candidate == kInvalidSocket) {
                continue;
            }

            if (::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
                handle_ = candidate;
                configure_timeouts();
                return;
            }

            close_socket(candidate);
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
        auto response = request(method, uri, std::move(headers), std::move(body));
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
            const auto timestamp = static_cast<uint32_t>(packet.presentation_timestamp & 0xFFFFFFFFu);
            auto header = make_rtp_header(
                kRtpPayloadTypeAirPlay2Realtime,
                ap2_first_audio_,
                next_rtp_sequence_++,
                timestamp,
                ap2_session_id_ == 0 ? kDefaultSsrc : ap2_session_id_);
            ap2_first_audio_ = false;

            const auto alac = make_alac_uncompressed_frame(frames, bytes);
            const auto encrypted = encrypt_airplay2_audio_payload(ap2_audio_key_, ap2_audio_nonce_++, header, alac);
            header.insert(header.end(), encrypted.begin(), encrypted.end());
            udp_ports_.data.send_to(remote_host_, remote_data_port_, header);
            return;
        }

        const auto timestamp = static_cast<uint32_t>(packet.presentation_timestamp & 0xFFFFFFFFu);
        const auto rtp = make_rtp_l16_packet(next_rtp_sequence_++, timestamp, kDefaultSsrc, frames, bytes);
        udp_ports_.data.send_to(remote_host_, remote_data_port_, rtp);
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

        request_success(
            "FLUSH",
            stream_uri_,
            {
                {"Session", rtsp_session_id},
            });
    }

    AirPlayNegotiatedSession open_airplay2(const OutputDevice& output, const PcmFormat& format) {
        if (format.sample_rate != 44100 || format.channels != 2 || format.bits_per_sample != 16) {
            throw std::invalid_argument("AirPlay 2 MVP currently requires 44.1 kHz stereo 16-bit PCM.");
        }

        dacp_id_ = random_hex(8);
        active_remote_ = std::to_string(random_u32());
        ap2_session_id_ = random_u32();
        if (ap2_session_id_ == 0) {
            ap2_session_id_ = kDefaultSsrc;
        }

        connect_to(output.endpoint_host, output.endpoint_port);
        bind_transport_ports();

        establish_airplay2_security(output);

        const auto info_response = request_success(
            "GET",
            "/info",
            ap2_headers());
        static_cast<void>(info_response);

        const auto local_ip = local_address_text();
        stream_uri_ = "rtsp://" + local_ip + "/" + std::to_string(ap2_session_id_);
        set_stream_uri(stream_uri_);

        const auto session_body = make_ap2_session_setup_body();
        const auto session_setup = request_success(
            "SETUP",
            stream_uri_,
            ap2_headers({
                {"Content-Type", "application/x-apple-binary-plist"},
                {"X-Apple-StreamID", "1"},
            }),
            string_from_bytes(session_body));

        const auto event_port = parse_ap2_event_port(session_setup.body);
        if (event_port != 0) {
            connect_event_channel(output.endpoint_host, event_port);
        }

        const auto record_response = request(
            "RECORD",
            stream_uri_,
            ap2_headers());
        if (!record_response.successful()) {
            // Some receivers still accept the stream SETUP after RECORD errors while they settle the event channel.
        }

        const auto stream_body = make_ap2_stream_setup_body();
        const auto stream_setup = request_success(
            "SETUP",
            stream_uri_,
            ap2_headers({
                {"Content-Type", "application/x-apple-binary-plist"},
                {"X-Apple-StreamID", "1"},
            }),
            string_from_bytes(stream_body));

        const auto stream_ports = parse_ap2_stream_ports(stream_setup.body);
        if (stream_ports.first == 0) {
            throw std::runtime_error("AirPlay 2 stream SETUP did not return a data port.");
        }
        remote_data_port_ = stream_ports.first;

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
        const auto event_handle = event_handle_;
        close_socket(event_handle);
        if (event_thread_.joinable()) {
            event_thread_.join();
        }
        event_handle_ = kInvalidSocket;
        close_socket(handle_);
        handle_ = kInvalidSocket;
#ifdef _WIN32
        winsock_.reset();
#endif
    }

private:
    void establish_airplay2_security(const OutputDevice& output) {
        const auto credentials = credential_store_->find(output);
        if (credentials) {
            run_pair_verify(*credentials);
            return;
        }

        const auto shared_secret = run_transient_pair_setup();
        apply_airplay2_security(shared_secret, derive_airplay2_encrypted_keys(shared_secret));
    }

    void apply_airplay2_security(const AirPlay2Bytes& shared_secret, const AirPlay2EncryptedKeys& keys) {
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
    }

    std::map<std::string, std::string> ap2_headers(std::map<std::string, std::string> headers = {}) const {
        headers.emplace("User-Agent", "AirPlay/550.10");
        headers.emplace("DACP-ID", dacp_id_);
        headers.emplace("Active-Remote", active_remote_);
        headers.emplace("Client-Instance", dacp_id_);
        headers.emplace("X-Apple-Client-Name", "FoobarUniversalMultiroom");
        return headers;
    }

    AirPlayRtspResponse http_post_success(
        const std::string& uri,
        const std::string& content_type,
        const AirPlay2Bytes& body) {
        std::ostringstream request;
        request << "POST " << uri << " HTTP/1.1\r\n";
        request << "CSeq: " << next_cseq_++ << "\r\n";
        request << "User-Agent: AirPlay/550.10\r\n";
        request << "Connection: keep-alive\r\n";
        request << "X-Apple-HKP: 4\r\n";
        request << "DACP-ID: " << dacp_id_ << "\r\n";
        request << "Active-Remote: " << active_remote_ << "\r\n";
        request << "Client-Instance: " << dacp_id_ << "\r\n";
        request << "X-Apple-Client-Name: FoobarUniversalMultiroom\r\n";
        request << "Content-Type: " << content_type << "\r\n";
        request << "Content-Length: " << body.size() << "\r\n\r\n";
        request << string_from_bytes(body);

        send_all(request.str());
        auto response = read_response();
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

    void run_pair_verify(const AirPlay2Credentials& credentials) {
        if (!credentials.valid_for_pair_verify()) {
            throw std::runtime_error("AirPlay 2 stored credentials are incomplete; pair this speaker again.");
        }

        AirPlay2PairVerifySession verifier(credentials.controller_identifier, credentials.controller_seed);
        const auto m2_response = http_post_success("/pair-verify", "application/octet-stream", verifier.make_m1());
        const auto m2 = verifier.handle_m2(bytes_from_string(m2_response.body), credentials.accessory_public_key);
        const auto m4_response = http_post_success("/pair-verify", "application/octet-stream", verifier.make_m3());
        validate_pair_verify_m4(bytes_from_string(m4_response.body));
        apply_airplay2_security(m2.shared_secret, m2.keys);
    }

    void validate_pair_verify_m4(const AirPlay2Bytes& body) const {
        using namespace fxchain::airplay;

        const auto message = tlv::decode(body);
        if (auto error = tlv::get(message, tlv::Error)) {
            throw std::runtime_error("AirPlay 2 pair-verify M4 returned error " +
                                     std::to_string(error->empty() ? 0 : error->front()) + ".");
        }

        const auto state = tlv::get(message, tlv::State);
        if (!state || state->size() != 1 || state->front() != 0x04) {
            throw std::runtime_error("AirPlay 2 pair-verify M4 was incomplete.");
        }
    }

    AirPlay2Bytes run_transient_pair_setup() {
        using namespace fxchain::airplay;

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

        Dict dict;
        const auto mac = random_mac_text();
        dict.emplace_back("deviceID", Value::str(mac));
        dict.emplace_back("sessionUUID", Value::str(random_uuid_text()));
        dict.emplace_back("timingPort", Value::integer(local_udp_ports().timing.port()));
        dict.emplace_back("timingProtocol", Value::str("NTP"));
        dict.emplace_back("isMultiSelectAirPlay", Value::boolean(true));
        dict.emplace_back("groupContainsGroupLeader", Value::boolean(false));
        dict.emplace_back("macAddress", Value::str(mac));
        dict.emplace_back("model", Value::str("iPhone14,3"));
        dict.emplace_back("name", Value::str("Foobar Universal Multiroom"));
        dict.emplace_back("osBuildVersion", Value::str("20F66"));
        dict.emplace_back("osName", Value::str("iPhone OS"));
        dict.emplace_back("osVersion", Value::str("16.5"));
        dict.emplace_back("senderSupportsRelay", Value::boolean(false));
        dict.emplace_back("sourceVersion", Value::str("690.7.1"));
        dict.emplace_back("statsCollectionEnabled", Value::boolean(false));
        return fxchain::airplay::bplist::encode(Value::object(std::move(dict)));
    }

    AirPlay2Bytes make_ap2_stream_setup_body() const {
        using namespace fxchain::airplay::bplist;

        Dict stream;
        stream.emplace_back("audioFormat", Value::integer(0x40000));
        stream.emplace_back("audioMode", Value::str("default"));
        stream.emplace_back("controlPort", Value::integer(local_udp_ports().control.port()));
        stream.emplace_back("ct", Value::integer(2));
        stream.emplace_back("isMedia", Value::boolean(true));
        stream.emplace_back("latencyMax", Value::integer(88200));
        stream.emplace_back("latencyMin", Value::integer(11025));
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

        configure_socket_timeouts(handle_, 3000);
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
    uint16_t remote_data_port_ = 0;
    uint32_t ap2_session_id_ = 0;
    uint64_t ap2_audio_nonce_ = 0;
    bool ap2_first_audio_ = true;
    std::string remote_host_;
    std::string stream_uri_;
    std::string dacp_id_;
    std::string active_remote_;
    AirPlay2Bytes ap2_audio_key_;
    std::unique_ptr<AirPlay2FrameCipher> control_cipher_;
    std::unique_ptr<AirPlay2FrameCipher> event_cipher_;
    std::atomic_bool event_running_ = false;
    std::thread event_thread_;
    socket_handle_t event_handle_ = kInvalidSocket;
    LocalUdpPorts udp_ports_;
    std::shared_ptr<AirPlay2CredentialStore> credential_store_;
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

AirPlayRtspControlClient::~AirPlayRtspControlClient() = default;

AirPlayRtspControlClient::AirPlayRtspControlClient(std::shared_ptr<AirPlay2CredentialStore> credential_store)
    : credential_store_(std::move(credential_store)) {
    if (!credential_store_) {
        throw std::invalid_argument("AirPlay 2 credential store cannot be null.");
    }
}

AirPlayNegotiatedSession AirPlayRtspControlClient::open(const OutputDevice& output, const PcmFormat& format) {
    auto connection = std::make_unique<Connection>(credential_store_);

    if (output.supports_airplay2) {
        auto session = connection->open_airplay2(output, format);
        connections_[output.id] = std::move(connection);
        return session;
    }

    connection->connect_to(output.endpoint_host, output.endpoint_port);

    const auto options_response = connection->request_success("OPTIONS", "*");
    const auto stream_uri = make_stream_uri(output);
    connection->set_stream_uri(stream_uri);
    const auto sdp = make_announce_sdp(output, format);
    connection->request_success(
        "ANNOUNCE",
        stream_uri,
        {
            {"Content-Type", "application/sdp"},
        },
        sdp);

    connection->bind_transport_ports();
    const auto local_ports = connection->local_udp_ports().to_transport_ports();
    const auto transport_header =
        "RTP/AVP/UDP;unicast;mode=record;client_port=" +
        std::to_string(local_ports.local_data_port) + "-" + std::to_string(local_ports.local_control_port) +
        ";control_port=" + std::to_string(local_ports.local_control_port) +
        ";timing_port=" + std::to_string(local_ports.local_timing_port);

    const auto setup_response = connection->request_success(
        "SETUP",
        stream_uri + "/streamid=0",
        {
            {"Transport", transport_header},
        });

    auto rtsp_session_id = session_id_from_header(setup_response.header("Session"));
    if (rtsp_session_id.empty()) {
        rtsp_session_id = session_id_from_header(options_response.header("Session"));
    }
    if (rtsp_session_id.empty()) {
        throw std::runtime_error("AirPlay RTSP SETUP did not return a session id.");
    }

    connection->request_success(
        "RECORD",
        stream_uri,
        {
            {"Range", "npt=0-"},
            {"RTP-Info", "seq=0;rtptime=0"},
            {"Session", rtsp_session_id},
        });

    const auto transport_parameters = parse_transport_parameters(setup_response.header("Transport"));
    const auto server_data_port = transport_port(transport_parameters, "server_port");
    if (server_data_port == 0) {
        throw std::runtime_error("AirPlay RTSP SETUP did not return a server RTP data port.");
    }
    connection->set_remote_data_endpoint(server_data_port);

    AirPlayNegotiatedSession session;
    session.rtsp_session_id = rtsp_session_id;
    session.stream_uri = stream_uri;
    session.server_name = options_response.header("Server");
    session.supported_methods = split_methods(options_response.header("Public"));
    session.ports = local_ports;
    session.ports.server_data_port = server_data_port;
    session.ports.server_control_port = transport_port(transport_parameters, "control_port");
    session.ports.server_timing_port = transport_port(transport_parameters, "timing_port");

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

std::shared_ptr<AirPlayControlClient> make_airplay_rtsp_control_client() {
    return std::make_shared<AirPlayRtspControlClient>();
}

std::shared_ptr<AirPlayControlClient> make_airplay_loopback_control_client() {
    return std::make_shared<AirPlayLoopbackControlClient>();
}

}  // namespace multiroom::airplay
