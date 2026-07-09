#pragma once

#include "airplay2_external_crypto.h"
#include "transport.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace multiroom::airplay {

struct AirPlay2Credentials {
    std::string output_id;
    std::string controller_identifier;
    AirPlay2Bytes controller_seed;
    AirPlay2Bytes accessory_identifier;
    AirPlay2Bytes accessory_public_key;

    bool valid_for_pair_verify() const;
};

class AirPlay2CredentialStore {
public:
    virtual ~AirPlay2CredentialStore() = default;

    virtual std::optional<AirPlay2Credentials> find(const OutputDevice& output) = 0;
    virtual void save(AirPlay2Credentials credentials) = 0;
    virtual void remove(const std::string& output_id) = 0;
};

class AirPlay2MemoryCredentialStore final : public AirPlay2CredentialStore {
public:
    std::optional<AirPlay2Credentials> find(const OutputDevice& output) override;
    void save(AirPlay2Credentials credentials) override;
    void remove(const std::string& output_id) override;

private:
    std::mutex mutex_;
    std::map<std::string, AirPlay2Credentials> credentials_;
};

class AirPlay2FileCredentialStore final : public AirPlay2CredentialStore {
public:
    explicit AirPlay2FileCredentialStore(std::filesystem::path path);

    std::optional<AirPlay2Credentials> find(const OutputDevice& output) override;
    void save(AirPlay2Credentials credentials) override;
    void remove(const std::string& output_id) override;

private:
    std::map<std::string, AirPlay2Credentials> load_all() const;
    void write_all(const std::map<std::string, AirPlay2Credentials>& credentials) const;

    mutable std::mutex mutex_;
    std::filesystem::path path_;
};

std::shared_ptr<AirPlay2CredentialStore> make_airplay2_memory_credential_store();

}  // namespace multiroom::airplay
