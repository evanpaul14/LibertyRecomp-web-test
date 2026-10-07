/**
 * @file        rex/system/web_guest_access.h
 * @brief       Guest load/store helpers used by recompiled code on the web
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Natively, recompiled loads and stores are plain host memory accesses:
 * aliased guest ranges share pages, and MMIO is caught by faulting on
 * protected pages. WebAssembly has neither, so on the web every access
 *
 *   - folds aliased ranges onto one copy (rex/memory/web_guest_layout.h), and
 *   - sends accesses in 0x7F000000-0x7FFFFFFF to the MMIO handler first,
 *     falling back to memory for the parts of that block (GPU writeback)
 *     that have no registered MMIO range.
 *
 * MMIO values cross the handler in host byte order, matching the codegen's
 * REX_MM_LOAD/REX_MM_STORE macros; 64-bit accesses are split into two 32-bit
 * register accesses, high word first.
 */

#pragma once

#include <cstdint>
#include <type_traits>

#include <rex/memory/web_guest_layout.h>
#include <rex/system/mmio_handler.h>

namespace rex::web_guest {

inline uint8_t* HostAddress(uint8_t* base, uint32_t address) noexcept {
  return base + address + memory::web::HostOffset(address);
}

inline bool IsMmioBlock(uint32_t address) noexcept {
  return (address >> 24) == 0x7F;
}

template <typename T>
inline T ByteSwap(T value) noexcept {
  if constexpr (sizeof(T) == 1) {
    return value;
  } else if constexpr (sizeof(T) == 2) {
    return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(value)));
  } else if constexpr (sizeof(T) == 4) {
    return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(value)));
  } else {
    static_assert(sizeof(T) == 8);
    return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(value)));
  }
}

template <typename T>
[[gnu::noinline]] inline bool TryMmioLoad(uint32_t address, T* out) noexcept {
  auto* handler = runtime::MMIOHandler::global_handler();
  if (!handler) {
    return false;
  }
  if constexpr (sizeof(T) == 8) {
    uint32_t hi, lo;
    if (!handler->CheckLoad(address, &hi)) {
      return false;
    }
    handler->CheckLoad(address + 4, &lo);
    *out = static_cast<T>((static_cast<uint64_t>(hi) << 32) | lo);
  } else {
    uint32_t value;
    if (!handler->CheckLoad(address, &value)) {
      return false;
    }
    *out = static_cast<T>(value);
  }
  return true;
}

template <typename T>
[[gnu::noinline]] inline bool TryMmioStore(uint32_t address, T value) noexcept {
  auto* handler = runtime::MMIOHandler::global_handler();
  if (!handler) {
    return false;
  }
  if constexpr (sizeof(T) == 8) {
    auto v = static_cast<uint64_t>(value);
    if (!handler->CheckStore(address, static_cast<uint32_t>(v >> 32))) {
      return false;
    }
    handler->CheckStore(address + 4, static_cast<uint32_t>(v));
    return true;
  } else {
    return handler->CheckStore(address, static_cast<uint32_t>(value));
  }
}

// Loads a big-endian guest value and returns it in host byte order.
template <typename T>
inline T Load(uint8_t* base, uint32_t address) noexcept {
  static_assert(std::is_unsigned_v<T>);
  if (IsMmioBlock(address)) [[unlikely]] {
    T value;
    if (TryMmioLoad(address, &value)) {
      return value;
    }
  }
  return ByteSwap(*reinterpret_cast<volatile T*>(HostAddress(base, address)));
}

// Stores a host-order value to guest memory as big-endian.
template <typename T>
inline void Store(uint8_t* base, uint32_t address, T value) noexcept {
  static_assert(std::is_unsigned_v<T>);
  if (IsMmioBlock(address)) [[unlikely]] {
    if (TryMmioStore(address, value)) {
      return;
    }
  }
  *reinterpret_cast<volatile T*>(HostAddress(base, address)) = ByteSwap(value);
}

}  // namespace rex::web_guest
