#include "airplay2_external_crypto.h"

#include <airplay_crypto.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace multiroom::airplay {

namespace {

constexpr size_t kAirPlay2FrameChunkSize = 1024;
constexpr size_t kAirPlay2TagSize = 16;

AirPlay2Bytes string_bytes(const std::string& value) {
    return AirPlay2Bytes(value.begin(), value.end());
}

AirPlay2Bytes le16(uint16_t value) {
    return {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
    };
}

uint16_t read_le16(const AirPlay2Bytes& bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

AirPlay2Bytes hkdf(const std::string& salt, const std::string& info, const AirPlay2Bytes& input) {
    return fxchain::airplay::hkdfSha512(salt, info, input, 32);
}

AirPlay2Bytes encrypt_with_named_nonce(
    const AirPlay2Bytes& key,
    const char* nonce,
    const AirPlay2Bytes& plaintext) {
    return fxchain::airplay::chacha20Poly1305Encrypt(key, string_bytes(nonce), plaintext, {});
}

AirPlay2Bytes decrypt_with_named_nonce(
    const AirPlay2Bytes& key,
    const char* nonce,
    const AirPlay2Bytes& ciphertext) {
    auto plaintext = fxchain::airplay::chacha20Poly1305Decrypt(key, string_bytes(nonce), ciphertext, {});
    if (!plaintext) {
        throw std::runtime_error("AirPlay 2 encrypted pairing message failed authentication.");
    }
    return *plaintext;
}

AirPlay2Bytes tlv_get_required(const fxchain::airplay::tlv::Map& tlv, uint8_t type, const char* name) {
    auto value = fxchain::airplay::tlv::get(tlv, type);
    if (!value) {
        throw std::runtime_error(std::string("AirPlay 2 TLV response is missing ") + name + ".");
    }
    return *value;
}

void tlv_require_state(const fxchain::airplay::tlv::Map& tlv, uint8_t expected) {
    const auto state = tlv_get_required(tlv, fxchain::airplay::tlv::State, "state");
    if (state.size() != 1 || state.front() != expected) {
        throw std::runtime_error("AirPlay 2 TLV response has an unexpected state.");
    }
}

bool verify_accessory_signature(
    const AirPlay2Bytes& accessory_public_key,
    const AirPlay2Bytes& accessory_identifier,
    const AirPlay2Bytes& controller_public_key,
    const AirPlay2Bytes& expected_accessory_public_key,
    const AirPlay2Bytes& signature) {
    AirPlay2Bytes signed_payload;
    signed_payload.reserve(accessory_public_key.size() + accessory_identifier.size() + controller_public_key.size());
    signed_payload.insert(signed_payload.end(), accessory_public_key.begin(), accessory_public_key.end());
    signed_payload.insert(signed_payload.end(), accessory_identifier.begin(), accessory_identifier.end());
    signed_payload.insert(signed_payload.end(), controller_public_key.begin(), controller_public_key.end());
    return fxchain::airplay::ed25519Verify(expected_accessory_public_key, signed_payload, signature);
}

}  // namespace

AirPlay2FrameCipher::AirPlay2FrameCipher(AirPlay2Bytes write_key, AirPlay2Bytes read_key)
    : write_key_(std::move(write_key))
    , read_key_(std::move(read_key)) {
    if (write_key_.size() != 32 || read_key_.size() != 32) {
        throw std::invalid_argument("AirPlay 2 encrypted channel keys must be 32 bytes.");
    }
}

AirPlay2Bytes AirPlay2FrameCipher::encrypt_frame(const AirPlay2Bytes& plaintext) {
    AirPlay2Bytes framed;
    size_t offset = 0;
    while (offset < plaintext.size()) {
        const auto chunk_size = std::min(kAirPlay2FrameChunkSize, plaintext.size() - offset);
        const auto header = le16(static_cast<uint16_t>(chunk_size));
        const AirPlay2Bytes chunk(plaintext.begin() + static_cast<std::ptrdiff_t>(offset),
                                  plaintext.begin() + static_cast<std::ptrdiff_t>(offset + chunk_size));
        const auto encrypted = fxchain::airplay::chacha20Poly1305Encrypt(
            write_key_,
            fxchain::airplay::counterNonce8(send_counter_++),
            chunk,
            header);
        framed.insert(framed.end(), header.begin(), header.end());
        framed.insert(framed.end(), encrypted.begin(), encrypted.end());
        offset += chunk_size;
    }
    return framed;
}

std::vector<AirPlay2Bytes> AirPlay2FrameCipher::decrypt_available(const AirPlay2Bytes& bytes) {
    encrypted_buffer_.insert(encrypted_buffer_.end(), bytes.begin(), bytes.end());

    std::vector<AirPlay2Bytes> frames;
    size_t offset = 0;
    while (encrypted_buffer_.size() - offset >= 2) {
        const auto payload_size = read_le16(encrypted_buffer_, offset);
        const auto frame_size = static_cast<size_t>(2) + payload_size + kAirPlay2TagSize;
        if (encrypted_buffer_.size() - offset < frame_size) {
            break;
        }

        const AirPlay2Bytes header(encrypted_buffer_.begin() + static_cast<std::ptrdiff_t>(offset),
                                   encrypted_buffer_.begin() + static_cast<std::ptrdiff_t>(offset + 2));
        const AirPlay2Bytes encrypted(encrypted_buffer_.begin() + static_cast<std::ptrdiff_t>(offset + 2),
                                      encrypted_buffer_.begin() + static_cast<std::ptrdiff_t>(offset + frame_size));
        auto plaintext = fxchain::airplay::chacha20Poly1305Decrypt(
            read_key_,
            fxchain::airplay::counterNonce8(receive_counter_++),
            encrypted,
            header);
        if (!plaintext) {
            throw std::runtime_error("AirPlay 2 encrypted channel frame failed authentication.");
        }
        frames.push_back(std::move(*plaintext));
        offset += frame_size;
    }

    if (offset > 0) {
        encrypted_buffer_.erase(
            encrypted_buffer_.begin(),
            encrypted_buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return frames;
}

AirPlay2PairVerifySession::AirPlay2PairVerifySession(
    std::string controller_identifier,
    AirPlay2Bytes controller_seed)
    : controller_identifier_(std::move(controller_identifier))
    , controller_seed_(std::move(controller_seed)) {
    if (controller_identifier_.empty()) {
        throw std::invalid_argument("AirPlay 2 controller identifier cannot be empty.");
    }
    if (controller_seed_.size() != 32) {
        throw std::invalid_argument("AirPlay 2 controller seed must be 32 bytes.");
    }

    const auto key_pair = fxchain::airplay::x25519Generate();
    controller_public_key_ = key_pair.pub;
    verify_private_key_ = key_pair.priv;
}

const AirPlay2Bytes& AirPlay2PairVerifySession::controller_public_key() const {
    return controller_public_key_;
}

AirPlay2Bytes AirPlay2PairVerifySession::make_m1() {
    return fxchain::airplay::tlv::encode({
        {fxchain::airplay::tlv::State, {0x01}},
        {fxchain::airplay::tlv::PublicKey, controller_public_key_},
    });
}

AirPlay2PairVerifyM2 AirPlay2PairVerifySession::handle_m2(
    const AirPlay2Bytes& response,
    const std::optional<AirPlay2Bytes>& expected_accessory_public_key) {
    const auto tlv = fxchain::airplay::tlv::decode(response);
    tlv_require_state(tlv, 0x02);

    accessory_public_key_ = tlv_get_required(tlv, fxchain::airplay::tlv::PublicKey, "accessory public key");
    const auto encrypted = tlv_get_required(tlv, fxchain::airplay::tlv::EncryptedData, "encrypted data");

    shared_secret_ = fxchain::airplay::x25519SharedSecret(verify_private_key_, accessory_public_key_);
    verify_key_ = hkdf("Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", shared_secret_);
    const auto decrypted = decrypt_with_named_nonce(verify_key_, "PV-Msg02", encrypted);
    const auto sub_tlv = fxchain::airplay::tlv::decode(decrypted);

    AirPlay2PairVerifyM2 result;
    result.accessory_identifier = tlv_get_required(sub_tlv, fxchain::airplay::tlv::Identifier, "accessory identifier");
    result.accessory_public_key = accessory_public_key_;
    result.shared_secret = shared_secret_;
    result.keys = derive_airplay2_encrypted_keys(shared_secret_);

    const auto signature = tlv_get_required(sub_tlv, fxchain::airplay::tlv::Signature, "accessory signature");
    if (expected_accessory_public_key) {
        result.accessory_signature_verified = verify_accessory_signature(
            accessory_public_key_,
            result.accessory_identifier,
            controller_public_key_,
            *expected_accessory_public_key,
            signature);
        if (!result.accessory_signature_verified) {
            throw std::runtime_error("AirPlay 2 pair-verify accessory signature mismatch.");
        }
    }

    return result;
}

AirPlay2Bytes AirPlay2PairVerifySession::make_m3() {
    if (accessory_public_key_.empty() || shared_secret_.empty() || verify_key_.empty()) {
        throw std::logic_error("AirPlay 2 pair-verify M3 requested before M2 was handled.");
    }

    const auto controller_public_long_term = fxchain::airplay::ed25519PublicFromSeed(controller_seed_);
    static_cast<void>(controller_public_long_term);

    AirPlay2Bytes signed_payload;
    signed_payload.reserve(controller_public_key_.size() + controller_identifier_.size() + accessory_public_key_.size());
    signed_payload.insert(signed_payload.end(), controller_public_key_.begin(), controller_public_key_.end());
    signed_payload.insert(signed_payload.end(), controller_identifier_.begin(), controller_identifier_.end());
    signed_payload.insert(signed_payload.end(), accessory_public_key_.begin(), accessory_public_key_.end());

    const auto signature = fxchain::airplay::ed25519Sign(controller_seed_, signed_payload);
    const auto sub_tlv = fxchain::airplay::tlv::encode({
        {fxchain::airplay::tlv::Identifier, string_bytes(controller_identifier_)},
        {fxchain::airplay::tlv::Signature, signature},
    });

    const auto encrypted = encrypt_with_named_nonce(verify_key_, "PV-Msg03", sub_tlv);
    return fxchain::airplay::tlv::encode({
        {fxchain::airplay::tlv::State, {0x03}},
        {fxchain::airplay::tlv::EncryptedData, encrypted},
    });
}

AirPlay2EncryptedKeys derive_airplay2_encrypted_keys(const AirPlay2Bytes& shared_secret) {
    AirPlay2EncryptedKeys keys;
    keys.control_write = hkdf("Control-Salt", "Control-Write-Encryption-Key", shared_secret);
    keys.control_read = hkdf("Control-Salt", "Control-Read-Encryption-Key", shared_secret);
    keys.event_write = hkdf("Events-Salt", "Events-Write-Encryption-Key", shared_secret);
    keys.event_read = hkdf("Events-Salt", "Events-Read-Encryption-Key", shared_secret);
    return keys;
}

}  // namespace multiroom::airplay
