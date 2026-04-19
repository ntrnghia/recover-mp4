#pragma once
/// Shared constants and low-level helpers for MP4 recovery.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace recover {

/// 4-byte length prefix (=2) + NAL type 9 (Access Unit Delimiter)
inline constexpr std::array<uint8_t, 5> AUD_PATTERN = {0x00, 0x00, 0x00, 0x02, 0x09};

/// Bitmask for O(1) NAL type validation (bits 1..12 set).
/// Valid types: 1-12 for length-prefixed MP4 NAL units.
inline constexpr uint32_t VALID_NAL_MASK = 0x1FFE;

/// Maximum allowed size for a single NAL unit (8 MB).
inline constexpr uint32_t MAX_NAL_SIZE = 8'000'000;

/// Read a big-endian uint32_t from a raw pointer.
[[nodiscard]] inline constexpr uint32_t read_be32(const uint8_t* p) noexcept {
    uint32_t val = (static_cast<uint32_t>(p[0]) << 24)
                 | (static_cast<uint32_t>(p[1]) << 16)
                 | (static_cast<uint32_t>(p[2]) << 8)
                 |  static_cast<uint32_t>(p[3]);
    return val;
}

/// Read a big-endian uint64_t from a raw pointer.
[[nodiscard]] inline constexpr uint64_t read_be64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
}

/// Read a big-endian uint16_t from a raw pointer.
[[nodiscard]] inline constexpr uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

/// Write a big-endian uint32_t to a raw pointer.
inline constexpr void write_be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

/// Write a big-endian uint64_t to a raw pointer.
inline constexpr void write_be64(uint8_t* p, uint64_t v) noexcept {
    write_be32(p, static_cast<uint32_t>(v >> 32));
    write_be32(p + 4, static_cast<uint32_t>(v));
}

/// Write a big-endian uint16_t to a raw pointer.
inline constexpr void write_be16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

/// Parse slice_type from first 4 bytes of slice header payload.
/// Inlined exp-Golomb decoding — no function call overhead.
/// Returns: 0/5=P, 1/6=B, 2/7=I, or -1 on error.
[[nodiscard]] inline constexpr int parse_slice_type(const uint8_t* data) noexcept {
    uint32_t bits = read_be32(data);
    int bp = 31;

    // Skip first_mb_in_slice (exp-Golomb, value discarded)
    int z = 0;
    while (bp >= 0 && !(bits & (1u << bp))) {
        ++z;
        --bp;
        if (z > 16) return -1;
    }
    if (bp < 0) return -1;
    bp -= 1 + z; // skip stop-bit + z value bits
    if (bp < 0) return -1;

    // Read slice_type (exp-Golomb)
    z = 0;
    while (bp >= 0 && !(bits & (1u << bp))) {
        ++z;
        --bp;
        if (z > 16) return -1;
    }
    if (bp < 0) return -1;
    --bp; // skip stop-bit

    uint32_t val = 0;
    for (int i = 0; i < z; ++i) {
        if (bp < 0) return -1;
        val = (val << 1) | ((bits >> bp) & 1);
        --bp;
    }
    return static_cast<int>((1u << z) - 1 + val);
}

} // namespace recover
