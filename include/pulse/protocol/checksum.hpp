#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pulse::protocol {

// CRC32C (Castagnoli). Uses the hardware instruction when the target has it
// (ARMv8 CRC extension, x86 SSE4.2) and a table-driven fallback otherwise.
// Both paths produce identical results; tests check them against each other.
std::uint32_t crc32c(std::uint32_t crc, std::span<const std::byte> data) noexcept;
std::uint32_t crc32c_software(std::uint32_t crc, std::span<const std::byte> data) noexcept;
bool crc32c_hardware_available() noexcept;

}  // namespace pulse::protocol
