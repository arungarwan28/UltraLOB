// UltraLOB — CRC64 (ECMA-182 polynomial) used for deterministic state checksums.
//
// The checksum is computed over a canonical byte serialization of book state
// (little endian, fixed field order). Any divergence in price, level totals,
// FIFO order or remaining quantity changes the checksum, which is what makes
// replay comparison meaningful rather than cosmetic.
#pragma once

#include <cstdint>
#include <cstring>

namespace ultralob {

namespace detail {

struct Crc64Table {
  std::uint64_t entries[256]{};

  constexpr Crc64Table() {
    constexpr std::uint64_t kPoly = 0x42F0E1EBA9EA3693ull;  // ECMA-182
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint64_t crc = static_cast<std::uint64_t>(i) << 56;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000000000000000ull) ? ((crc << 1) ^ kPoly) : (crc << 1);
      }
      entries[i] = crc;
    }
  }
};

inline constexpr Crc64Table kCrc64Table{};

}  // namespace detail

// Incremental CRC64-ECMA-182. Start with kCrc64Init.
//
// The initial value is non-zero on purpose: with the ECMA polynomial a table
// entry of zero maps zero bytes to zero, so starting at 0 would make the empty
// book hash to 0 and collapse every all-zero region to a single value.
inline constexpr std::uint64_t kCrc64Init = 0xFFFFFFFFFFFFFFFFull;

inline std::uint64_t crc64_update(std::uint64_t crc, const void* data,
                                  std::size_t len) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < len; ++i) {
    const std::uint8_t index =
        static_cast<std::uint8_t>(((crc >> 56) ^ bytes[i]) & 0xFFu);
    crc = detail::kCrc64Table.entries[index] ^ (crc << 8);
  }
  return crc;
}

// Explicit little-endian serialisation: hashing the raw object representation
// would make the checksum host-endian dependent, and a checksum that changes
// with the platform is not a determinism guarantee. All engine fields are
// fixed-width integers, so this is total.
inline std::uint64_t crc64_update_u64(std::uint64_t crc,
                                      std::uint64_t value) noexcept {
  std::uint8_t bytes[8];
  for (int i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  return crc64_update(crc, bytes, sizeof(bytes));
}

inline std::uint64_t crc64(const void* data, std::size_t len) noexcept {
  return crc64_update(kCrc64Init, data, len);
}

}  // namespace ultralob
