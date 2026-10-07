/**
 * @file        rex/memory/web_guest_layout.h
 * @brief       Guest address folding for the WebAssembly build
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Natively, rex::memory::Memory maps several guest ranges onto the same
 * backing pages (see map_info in src/system/xmemory.cpp):
 *
 *   0x7F000000-0x7FFFFFFF  -> physical 0x00000000 (GPU writeback)
 *   0xA0000000-0xBFFFFFFF  -> physical 0x00000000 (64 KB pages)
 *   0xC0000000-0xDFFFFFFF  -> physical 0x00000000 (16 MB pages)
 *   0xE0000000-0xFFFFFFFF  -> physical 0x00001000 (4 KB pages)
 *
 * WebAssembly linear memory cannot alias pages, so the web build keeps a
 * single copy of physical memory at kPhysicalBase (right after the 4 GB
 * virtual window, the same place physical_membase() points natively) and
 * redirects each aliased guest range to it. Every guest address therefore
 * maps to host `membase + address + HostOffset(address)`.
 *
 * The 0x90000000 XEX view, which natively mirrors 0x80000000, keeps its own
 * backing: folding it would need a negative offset, and nothing is known to
 * rely on that mirror.
 */

#pragma once

#include <array>
#include <cstdint>

namespace rex::memory::web {

inline constexpr uint64_t kPhysicalBase = 0x100000000ull;
inline constexpr uint64_t kPhysicalSize = 0x20000000ull;
// The 0xE0000000 view starts 4 KB into physical memory, so its last page
// lands just past the physical range.
inline constexpr uint64_t kRegionSize = kPhysicalBase + kPhysicalSize + 0x1000;

// Host offset (beyond `membase + address`) for the 16 MB guest block
// `address >> 24`.
constexpr uint64_t HostOffsetForBlock(uint32_t block) noexcept {
  if (block == 0x7F) {
    return kPhysicalBase - 0x7F000000ull;
  }
  if (block >= 0xA0 && block < 0xC0) {
    return kPhysicalBase - 0xA0000000ull;
  }
  if (block >= 0xC0 && block < 0xE0) {
    return kPhysicalBase - 0xC0000000ull;
  }
  if (block >= 0xE0) {
    return kPhysicalBase + 0x1000ull - 0xE0000000ull;
  }
  return 0;
}

inline constexpr std::array<uint64_t, 256> kHostOffsets = [] {
  std::array<uint64_t, 256> table{};
  for (uint32_t block = 0; block < table.size(); ++block) {
    table[block] = HostOffsetForBlock(block);
  }
  return table;
}();

constexpr uint64_t HostOffset(uint32_t guest_address) noexcept {
  return kHostOffsets[guest_address >> 24];
}

// The guest address a host pointer in the folded region refers to. Aliased
// ranges share host memory, so physical memory is reported through its
// canonical 0xA0000000 view.
constexpr uint32_t GuestAddressForHostOffset(uint64_t host_offset) noexcept {
  if (host_offset >= kPhysicalBase) {
    return static_cast<uint32_t>(0xA0000000ull + (host_offset - kPhysicalBase));
  }
  return static_cast<uint32_t>(host_offset);
}

static_assert(HostOffset(0xA0000000u) == HostOffset(0xBFFFFFFFu));
static_assert(0xA0001234ull + HostOffset(0xA0001234u) == kPhysicalBase + 0x1234);
static_assert(0xC0001234ull + HostOffset(0xC0001234u) == kPhysicalBase + 0x1234);
static_assert(0xE0000234ull + HostOffset(0xE0000234u) == kPhysicalBase + 0x1234);
static_assert(0x7F000010ull + HostOffset(0x7F000010u) == kPhysicalBase + 0x10);
static_assert(0xFFFFFFFFull + HostOffset(0xFFFFFFFFu) < kRegionSize);
static_assert(HostOffset(0x82000000u) == 0 && HostOffset(0x7EFFFFFFu) == 0);
static_assert(GuestAddressForHostOffset(kPhysicalBase + 0x1234) == 0xA0001234u);

}  // namespace rex::memory::web
