#include "airplay2_credentials.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace multiroom::airplay {

namespace {

constexpr size_t kControllerSeedSize = 32;
constexpr size_t kAccessoryPublicKeySize = 32;

std::string bytes_to_hex(const AirPlay2Bytes& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(kHex[(byte >> 4) & 0x0F]);
        result.push_back(kHex[byte & 0x0F]);
    }
    return result;
}

AirPlay2Bytes string_to_bytes(const std::string& value) {
    return AirPlay2Bytes(value.begin(), value.end());
}

std::string bytes_to_string(const AirPlay2Bytes& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

int hex_value(char ch) {
    const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower >= '0' && lower <= '9') return lower - '0';
    if (lower >= 'a' && lower <= 'f') return lower - 'a' + 10;
    return -1;
}

AirPlay2Bytes hex_to_bytes(const std::string& hex) {
    if ((hex.size() % 2) != 0) {
        throw std::invalid_argument("AirPlay 2 credential hex field has an odd length.");
    }

    AirPlay2Bytes result;
    result.reserve(hex.size() / 2);
    for (size_t index = 0; index < hex.size(); index += 2) {
        const int high = hex_value(hex[index]);
        const int low = hex_value(hex[index + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("AirPlay 2 credential hex field contains a non-hex character.");
        }
        result.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return result;
}

std::vector<std::string> split_tab_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '\t')) {
        fields.push_back(std::move(field));
    }
    return fields;
}

AirPlay2Credentials parse_record(const std::string& line) {
    const auto fields = split_tab_fields(line);
    if (fields.size() != 6 || fields[0] != "v1") {
        throw std::invalid_argument("AirPlay 2 credential record has an unsupported format.");
    }

    AirPlay2Credentials credentials;
    credentials.output_id = bytes_to_string(hex_to_bytes(fields[1]));
    credentials.controller_identifier = bytes_to_string(hex_to_bytes(fields[2]));
    credentials.controller_seed = hex_to_bytes(fields[3]);
    credentials.accessory_identifier = hex_to_bytes(fields[4]);
    credentials.accessory_public_key = hex_to_bytes(fields[5]);
    if (!credentials.valid_for_pair_verify()) {
        throw std::invalid_argument("AirPlay 2 credential record is incomplete.");
    }
    return credentials;
}

std::string format_record(const AirPlay2Credentials& credentials) {
    if (!credentials.valid_for_pair_verify()) {
        throw std::invalid_argument("AirPlay 2 credentials are incomplete.");
    }

    std::ostringstream stream;
    stream << "v1\t"
           << bytes_to_hex(string_to_bytes(credentials.output_id)) << '\t'
           << bytes_to_hex(string_to_bytes(credentials.controller_identifier)) << '\t'
           << bytes_to_hex(credentials.controller_seed) << '\t'
           << bytes_to_hex(credentials.accessory_identifier) << '\t'
           << bytes_to_hex(credentials.accessory_public_key);
    return stream.str();
}

}  // namespace

bool AirPlay2Credentials::valid_for_pair_verify() const {
    return !output_id.empty() &&
           !controller_identifier.empty() &&
           controller_seed.size() == kControllerSeedSize &&
           !accessory_identifier.empty() &&
           accessory_public_key.size() == kAccessoryPublicKeySize;
}

std::optional<AirPlay2Credentials> AirPlay2MemoryCredentialStore::find(const OutputDevice& output) {
    std::lock_guard lock(mutex_);
    const auto it = credentials_.find(output.id);
    if (it == credentials_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void AirPlay2MemoryCredentialStore::save(AirPlay2Credentials credentials) {
    if (!credentials.valid_for_pair_verify()) {
        throw std::invalid_argument("AirPlay 2 credentials are incomplete.");
    }

    std::lock_guard lock(mutex_);
    credentials_[credentials.output_id] = std::move(credentials);
}

void AirPlay2MemoryCredentialStore::remove(const std::string& output_id) {
    std::lock_guard lock(mutex_);
    credentials_.erase(output_id);
}

AirPlay2FileCredentialStore::AirPlay2FileCredentialStore(std::filesystem::path path)
    : path_(std::move(path)) {
    if (path_.empty()) {
        throw std::invalid_argument("AirPlay 2 credential store path cannot be empty.");
    }
}

std::optional<AirPlay2Credentials> AirPlay2FileCredentialStore::find(const OutputDevice& output) {
    std::lock_guard lock(mutex_);
    const auto credentials = load_all();
    const auto it = credentials.find(output.id);
    if (it == credentials.end()) {
        return std::nullopt;
    }
    return it->second;
}

void AirPlay2FileCredentialStore::save(AirPlay2Credentials credentials) {
    if (!credentials.valid_for_pair_verify()) {
        throw std::invalid_argument("AirPlay 2 credentials are incomplete.");
    }

    std::lock_guard lock(mutex_);
    auto all = load_all();
    all[credentials.output_id] = std::move(credentials);
    write_all(all);
}

void AirPlay2FileCredentialStore::remove(const std::string& output_id) {
    std::lock_guard lock(mutex_);
    auto all = load_all();
    all.erase(output_id);
    write_all(all);
}

std::map<std::string, AirPlay2Credentials> AirPlay2FileCredentialStore::load_all() const {
    std::map<std::string, AirPlay2Credentials> result;

    std::ifstream input(path_);
    if (!input) {
        return result;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }

        auto credentials = parse_record(line);
        result[credentials.output_id] = std::move(credentials);
    }

    return result;
}

void AirPlay2FileCredentialStore::write_all(const std::map<std::string, AirPlay2Credentials>& credentials) const {
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(path_, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not open AirPlay 2 credential store for writing.");
    }

    output << "# foobar-universal-multiroom AirPlay 2 credentials\n";
    for (const auto& [_, record] : credentials) {
        output << format_record(record) << '\n';
    }
}

std::shared_ptr<AirPlay2CredentialStore> make_airplay2_memory_credential_store() {
    return std::make_shared<AirPlay2MemoryCredentialStore>();
}

}  // namespace multiroom::airplay
