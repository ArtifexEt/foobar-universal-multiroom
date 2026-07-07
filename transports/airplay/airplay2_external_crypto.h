#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace multiroom::airplay {

using AirPlay2Bytes = std::vector<uint8_t>;

struct AirPlay2EncryptedKeys {
    AirPlay2Bytes control_write;
    AirPlay2Bytes control_read;
    AirPlay2Bytes event_write;
    AirPlay2Bytes event_read;
};

struct AirPlay2PairVerifyM2 {
    AirPlay2Bytes accessory_identifier;
    AirPlay2Bytes accessory_public_key;
    AirPlay2Bytes shared_secret;
    AirPlay2EncryptedKeys keys;
    bool accessory_signature_verified = false;
};

class AirPlay2FrameCipher {
public:
    AirPlay2FrameCipher(AirPlay2Bytes write_key, AirPlay2Bytes read_key);

    AirPlay2Bytes encrypt_frame(const AirPlay2Bytes& plaintext);
    std::vector<AirPlay2Bytes> decrypt_available(const AirPlay2Bytes& bytes);

private:
    AirPlay2Bytes write_key_;
    AirPlay2Bytes read_key_;
    AirPlay2Bytes encrypted_buffer_;
    uint64_t send_counter_ = 0;
    uint64_t receive_counter_ = 0;
};

class AirPlay2PairVerifySession {
public:
    AirPlay2PairVerifySession(std::string controller_identifier, AirPlay2Bytes controller_seed);

    const AirPlay2Bytes& controller_public_key() const;
    AirPlay2Bytes make_m1();
    AirPlay2PairVerifyM2 handle_m2(
        const AirPlay2Bytes& response,
        const std::optional<AirPlay2Bytes>& expected_accessory_public_key);
    AirPlay2Bytes make_m3();

private:
    std::string controller_identifier_;
    AirPlay2Bytes controller_seed_;
    AirPlay2Bytes controller_public_key_;
    AirPlay2Bytes verify_private_key_;
    AirPlay2Bytes accessory_public_key_;
    AirPlay2Bytes shared_secret_;
    AirPlay2Bytes verify_key_;
};

AirPlay2EncryptedKeys derive_airplay2_encrypted_keys(const AirPlay2Bytes& shared_secret);

}  // namespace multiroom::airplay
