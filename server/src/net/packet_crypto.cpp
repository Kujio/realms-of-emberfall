#include "net/packet_crypto.hpp"

#include <cstring>
#include <stdexcept>

#include <openssl/evp.h>
#include <sodium.h>

namespace ef::net {

namespace {

void put_u64_le(std::uint8_t* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint64_t get_u64_le(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return v;
}

}  // namespace

SessionKey random_session_key() {
  SessionKey k{};
  randombytes_buf(k.data(), k.size());
  return k;
}

std::string session_key_hex(const SessionKey& key) {
  std::string hex(key.size() * 2 + 1, '\0');
  sodium_bin2hex(hex.data(), hex.size(), key.data(), key.size());
  hex.pop_back();
  return hex;
}

bool parse_session_key_hex(std::string_view hex, SessionKey& out) {
  if (hex.size() != kSessionKeyBytes * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < kSessionKeyBytes; ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return true;
}

std::array<std::uint8_t, 32> derive_key(const SessionKey& key, std::string_view label) {
  std::array<std::uint8_t, 32> out{};
  crypto_auth_hmacsha256_state st;
  crypto_auth_hmacsha256_init(&st, key.data(), key.size());
  crypto_auth_hmacsha256_update(&st, reinterpret_cast<const unsigned char*>(label.data()), label.size());
  const unsigned char one = 0x01;
  crypto_auth_hmacsha256_update(&st, &one, 1);
  crypto_auth_hmacsha256_final(&st, out.data());
  sodium_memzero(&st, sizeof st);
  return out;
}

std::string cipher_label(bool client_to_server, std::uint8_t channel) {
  return std::string("emberfall/1 ") + (client_to_server ? "c2s " : "s2c ") + std::to_string(channel);
}

struct PacketCipher::Impl {
  EVP_CIPHER_CTX* ctx = nullptr;
  crypto_auth_hmacsha256_state mac_base{};  // keyed once; copied per packet

  Impl(const SessionKey& key, std::string_view label) {
    auto enc = derive_key(key, std::string(label) + " enc");
    auto mac = derive_key(key, std::string(label) + " mac");
    ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr || EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, enc.data(), nullptr) != 1) {
      if (ctx) EVP_CIPHER_CTX_free(ctx);
      throw std::runtime_error("AES-256-CTR is not available");
    }
    crypto_auth_hmacsha256_init(&mac_base, mac.data(), mac.size());
    sodium_memzero(enc.data(), enc.size());
    sodium_memzero(mac.data(), mac.size());
  }
  ~Impl() {
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    sodium_memzero(&mac_base, sizeof mac_base);
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  // CTR is its own inverse: the same call encrypts and decrypts. `out` has room for `n` bytes.
  bool crypt(std::uint64_t counter, const std::uint8_t* in, std::size_t n, std::uint8_t* out) {
    std::array<std::uint8_t, 16> iv{};
    put_u64_le(iv.data(), counter);
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, nullptr, iv.data()) != 1) return false;
    int len = 0;
    if (n > 0 && EVP_EncryptUpdate(ctx, out, &len, in, static_cast<int>(n)) != 1) return false;
    return static_cast<std::size_t>(len) == n;
  }

  void tag(const std::uint8_t* counter_and_ct, std::size_t n, std::uint8_t* out16) {
    crypto_auth_hmacsha256_state st = mac_base;
    crypto_auth_hmacsha256_update(&st, counter_and_ct, n);
    std::array<std::uint8_t, 32> full{};
    crypto_auth_hmacsha256_final(&st, full.data());
    std::memcpy(out16, full.data(), kSealTagBytes);
  }
};

PacketCipher::PacketCipher(const SessionKey& key, std::string_view label) : impl_(std::make_unique<Impl>(key, label)) {}
PacketCipher::~PacketCipher() = default;
PacketCipher::PacketCipher(PacketCipher&&) noexcept = default;
PacketCipher& PacketCipher::operator=(PacketCipher&&) noexcept = default;

void PacketCipher::seal(std::span<const std::uint8_t> plain, std::vector<std::uint8_t>& out) {
  const std::uint64_t counter = send_counter_++;
  out.resize(kSealCounterBytes + plain.size() + kSealTagBytes);
  put_u64_le(out.data(), counter);
  if (!impl_->crypt(counter, plain.data(), plain.size(), out.data() + kSealCounterBytes)) {
    throw std::runtime_error("AES-256-CTR failed");  // OpenSSL does not fail on valid input
  }
  impl_->tag(out.data(), kSealCounterBytes + plain.size(), out.data() + kSealCounterBytes + plain.size());
}

PacketCipher::Open PacketCipher::open(std::span<const std::uint8_t> sealed, std::vector<std::uint8_t>& out, bool in_order) {
  if (sealed.size() < kSealOverhead) return Open::Malformed;
  const std::size_t body = sealed.size() - kSealTagBytes;
  std::array<std::uint8_t, kSealTagBytes> want{};
  impl_->tag(sealed.data(), body, want.data());
  if (sodium_memcmp(want.data(), sealed.data() + body, kSealTagBytes) != 0) return Open::BadTag;
  const std::uint64_t counter = get_u64_le(sealed.data());
  if (in_order ? counter != recv_next_ : counter < recv_next_) return Open::Replay;
  const std::size_t n = body - kSealCounterBytes;
  out.resize(n);
  if (!impl_->crypt(counter, sealed.data() + kSealCounterBytes, n, out.data())) return Open::Malformed;
  recv_next_ = counter + 1;
  return Open::Ok;
}

SessionCrypto::SessionCrypto(const SessionKey& key, Role role) {
  const bool client = role == Role::Client;
  for (std::uint8_t ch = 0; ch < kChannels; ++ch) {
    send_.emplace_back(key, cipher_label(client, ch));
    recv_.emplace_back(key, cipher_label(!client, ch));
  }
}

void SessionCrypto::seal(std::uint8_t channel, std::span<const std::uint8_t> plain, std::vector<std::uint8_t>& out) {
  send_.at(channel).seal(plain, out);
}

PacketCipher::Open SessionCrypto::open(std::uint8_t channel, std::span<const std::uint8_t> sealed, std::vector<std::uint8_t>& out) {
  if (channel >= kChannels) return PacketCipher::Open::Malformed;
  return recv_[channel].open(sealed, out, channel == 0);
}

}  // namespace ef::net
