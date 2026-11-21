// Bounds-checked little-endian reader/writer used by the generated protocol code.
// Every Reader method returns false (and latches failure) instead of reading past the end.
#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ef::net {

// Returns true if `s` is well-formed UTF-8 (no overlongs, no surrogates, max U+10FFFF).
inline bool is_valid_utf8(std::string_view s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const std::size_t n = s.size();
  std::size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    std::size_t len = 0;
    std::uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (n - i < len) return false;
    for (std::size_t k = 1; k < len; ++k) {
      const unsigned char cc = p[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

// Largest prefix length <= max_bytes that does not split a UTF-8 sequence.
inline std::size_t utf8_truncate_len(std::string_view s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s.size();
  std::size_t len = max_bytes;
  while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) --len;
  return len;
}

class Writer {
 public:
  explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

  void u8(std::uint8_t v) { out_.push_back(v); }
  void u16(std::uint16_t v) { put_le(v); }
  void u32(std::uint32_t v) { put_le(v); }
  void u64(std::uint64_t v) { put_le(v); }
  void i32(std::int32_t v) { put_le(static_cast<std::uint32_t>(v)); }
  void f32(float v) {
    if (!std::isfinite(v)) v = 0.0f;
    put_le(std::bit_cast<std::uint32_t>(v));
  }
  void boolean(bool v) { out_.push_back(v ? 1 : 0); }
  // Strings longer than max_bytes are truncated on a UTF-8 boundary so output always decodes.
  void str(std::string_view s, std::size_t max_bytes) {
    const std::size_t len = utf8_truncate_len(s, max_bytes);
    u16(static_cast<std::uint16_t>(len));
    out_.insert(out_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(len));
  }

 private:
  template <class T>
  void put_le(T v) {
    for (std::size_t i = 0; i < sizeof(T); ++i) out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  std::vector<std::uint8_t>& out_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

  bool ok() const { return ok_; }
  bool at_end() const { return ok_ && pos_ == data_.size(); }
  std::size_t remaining() const { return data_.size() - pos_; }
  bool fail() {
    ok_ = false;
    return false;
  }

  bool u8(std::uint8_t& v) { return get_le(v); }
  bool u16(std::uint16_t& v) { return get_le(v); }
  bool u32(std::uint32_t& v) { return get_le(v); }
  bool u64(std::uint64_t& v) { return get_le(v); }
  bool i32(std::int32_t& v) {
    std::uint32_t u = 0;
    if (!get_le(u)) return false;
    v = static_cast<std::int32_t>(u);
    return true;
  }
  // Non-finite floats (NaN/Inf) are protocol violations.
  bool f32(float& v) {
    std::uint32_t u = 0;
    if (!get_le(u)) return false;
    v = std::bit_cast<float>(u);
    if (!std::isfinite(v)) return fail();
    return true;
  }
  bool boolean(bool& v) {
    std::uint8_t b = 0;
    if (!get_le(b)) return false;
    if (b > 1) return fail();
    v = b == 1;
    return true;
  }
  bool str(std::string& v, std::size_t max_bytes) {
    std::uint16_t len = 0;
    if (!get_le(len)) return false;
    if (len > max_bytes || len > remaining()) return fail();
    v.assign(reinterpret_cast<const char*>(data_.data() + pos_), len);
    pos_ += len;
    if (!is_valid_utf8(v)) return fail();
    return true;
  }

 private:
  template <class T>
  bool get_le(T& v) {
    if (!ok_ || remaining() < sizeof(T)) return fail();
    T out = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) out = static_cast<T>(out | (static_cast<T>(data_[pos_ + i]) << (8 * i)));
    pos_ += sizeof(T);
    v = out;
    return true;
  }

  std::span<const std::uint8_t> data_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace ef::net
