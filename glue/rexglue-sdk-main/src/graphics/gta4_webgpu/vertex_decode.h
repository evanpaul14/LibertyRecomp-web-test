#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rex::graphics::gta4_webgpu {

// Title vertex shaders read every attribute as vec4<f32>, while WebGPU only
// converts normalized formats to floats. Elements are decoded from guest
// (big-endian) bytes to float4 on the CPU, with D3D's (0, 0, 0, 1) defaults.
// Conversions match the Metal renderer's vertex descriptors.

inline uint16_t LoadBig16(const uint8_t* bytes) {
  return uint16_t(bytes[0] << 8 | bytes[1]);
}
inline uint32_t LoadBig32Vertex(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, bytes, sizeof(value));
  return __builtin_bswap32(value);
}
inline float HalfToFloat(uint16_t value) {
  const uint32_t sign = uint32_t(value & 0x8000u) << 16;
  uint32_t exponent = (value >> 10) & 0x1Fu;
  uint32_t mantissa = value & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (!mantissa) {
      bits = sign;
    } else {
      exponent = 127 - 15 + 1;
      while (!(mantissa & 0x400u)) {
        mantissa <<= 1;
        --exponent;
      }
      bits = sign | (exponent << 23) | ((mantissa & 0x3FFu) << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

// Bytes an element occupies in the guest stream; 0 for an unknown type.
inline uint32_t VertexElementSize(uint32_t type) {
  switch (type) {
    case 0x2C83A4: return 4;
    case 0x2C23A5: return 8;
    case 0x2A23B9: return 12;
    case 0x1A23A6: return 16;
    case 0x182886: case 0x1A2286: case 0x1A2386: case 0x1A2086: case 0x1A2186:
    case 0x2C2359: case 0x2C2159: case 0x2C2059: case 0x2C235F:
    case 0x2C82A1: case 0x2A2287: case 0x2A2187: case 0x2A2190: case 0x2A2390:
    case 0x1A2187: return 4;
    case 0x1A235A: case 0x1A215A: case 0x1A205A: case 0x1A2360: return 8;
    default: return 0;
  }
}

inline bool DecodeVertexElement(uint32_t type, const uint8_t* source, float* out) {
  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;
  switch (type) {
    case 0x2C83A4: case 0x2C23A5: case 0x2A23B9: case 0x1A23A6: {
      const uint32_t count = type == 0x2C83A4 ? 1 : type == 0x2C23A5 ? 2 : type == 0x2A23B9 ? 3 : 4;
      for (uint32_t i = 0; i < count; ++i)
        out[i] = std::bit_cast<float>(LoadBig32Vertex(source + i * 4));
      return true;
    }
    case 0x182886: {  // D3DCOLOR (ARGB word): normalized RGBA.
      const uint32_t word = LoadBig32Vertex(source);
      out[0] = float((word >> 16) & 0xFF) / 255.0f;
      out[1] = float((word >> 8) & 0xFF) / 255.0f;
      out[2] = float(word & 0xFF) / 255.0f;
      out[3] = float(word >> 24) / 255.0f;
      return true;
    }
    case 0x1A2286: case 0x1A2386: case 0x1A2086: case 0x1A2186: {  // UBYTE4 (word order).
      const uint32_t word = LoadBig32Vertex(source);
      const float scale = (type == 0x1A2086 || type == 0x1A2186) ? 1.0f / 255.0f : 1.0f;
      for (uint32_t i = 0; i < 4; ++i) out[i] = float((word >> (8 * i)) & 0xFF) * scale;
      return true;
    }
    case 0x2C2359: case 0x1A235A: {  // SHORT2/4.
      const uint32_t count = type == 0x2C2359 ? 2 : 4;
      for (uint32_t i = 0; i < count; ++i) out[i] = float(int16_t(LoadBig16(source + i * 2)));
      return true;
    }
    case 0x2C2159: case 0x1A215A: {  // SHORT2N/4N.
      const uint32_t count = type == 0x2C2159 ? 2 : 4;
      for (uint32_t i = 0; i < count; ++i)
        out[i] = std::max(float(int16_t(LoadBig16(source + i * 2))) / 32767.0f, -1.0f);
      return true;
    }
    case 0x2C2059: case 0x1A205A: {  // USHORT2N/4N.
      const uint32_t count = type == 0x2C2059 ? 2 : 4;
      for (uint32_t i = 0; i < count; ++i) out[i] = float(LoadBig16(source + i * 2)) / 65535.0f;
      return true;
    }
    case 0x2C235F: case 0x1A2360: {  // FLOAT16_2/4.
      const uint32_t count = type == 0x2C235F ? 2 : 4;
      for (uint32_t i = 0; i < count; ++i) out[i] = HalfToFloat(LoadBig16(source + i * 2));
      return true;
    }
    case 0x2C82A1: case 0x2A2287: case 0x2A2187: case 0x2A2190: case 0x2A2390:
      out[0] = float(LoadBig32Vertex(source));
      return true;
    case 0x1A2187: {  // DEC3N; the guest's w field is replaced by 1.
      const uint32_t word = LoadBig32Vertex(source);
      for (uint32_t i = 0; i < 3; ++i) {
        const int32_t value = int32_t(word << (22 - 10 * i)) >> 22;
        out[i] = std::max(float(value) / 511.0f, -1.0f);
      }
      out[3] = 1.0f;
      return true;
    }
    default:
      return false;
  }
}

}  // namespace rex::graphics::gta4_webgpu
