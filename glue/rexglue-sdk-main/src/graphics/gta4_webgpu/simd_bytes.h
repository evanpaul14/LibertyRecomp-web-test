#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <wasm_simd128.h>

namespace rex::graphics::gta4_webgpu {

// Emscripten's libc compares and copies these per byte; the renderer runs
// them over kilobytes of guest state for every draw.

// True when the two ranges hold the same bytes.
inline bool BytesEqual(const void* a, const void* b, size_t size) {
  const auto* x = static_cast<const uint8_t*>(a);
  const auto* y = static_cast<const uint8_t*>(b);
  size_t offset = 0;
  for (; offset + 64 <= size; offset += 64) {
    const v128_t d0 = wasm_v128_xor(wasm_v128_load(x + offset), wasm_v128_load(y + offset));
    const v128_t d1 =
        wasm_v128_xor(wasm_v128_load(x + offset + 16), wasm_v128_load(y + offset + 16));
    const v128_t d2 =
        wasm_v128_xor(wasm_v128_load(x + offset + 32), wasm_v128_load(y + offset + 32));
    const v128_t d3 =
        wasm_v128_xor(wasm_v128_load(x + offset + 48), wasm_v128_load(y + offset + 48));
    if (wasm_v128_any_true(wasm_v128_or(wasm_v128_or(d0, d1), wasm_v128_or(d2, d3))))
      return false;
  }
  for (; offset + 16 <= size; offset += 16) {
    if (wasm_v128_any_true(wasm_v128_xor(wasm_v128_load(x + offset), wasm_v128_load(y + offset))))
      return false;
  }
  return !std::memcmp(x + offset, y + offset, size - offset);
}

// Copies big-endian 32-bit words, swapping them to host order. A trailing
// partial word is copied as is.
inline void CopySwap32(uint8_t* destination, const uint8_t* source, size_t size) {
  size_t offset = 0;
  for (; offset + 16 <= size; offset += 16) {
    const v128_t words = wasm_v128_load(source + offset);
    wasm_v128_store(destination + offset,
                    wasm_i8x16_shuffle(words, words, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15,
                                       14, 13, 12));
  }
  for (; offset + 4 <= size; offset += 4) {
    uint32_t word;
    std::memcpy(&word, source + offset, sizeof(word));
    word = __builtin_bswap32(word);
    std::memcpy(destination + offset, &word, sizeof(word));
  }
  std::memcpy(destination + offset, source + offset, size - offset);
}

}  // namespace rex::graphics::gta4_webgpu
