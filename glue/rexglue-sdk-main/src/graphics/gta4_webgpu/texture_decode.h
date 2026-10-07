#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/texture/info.h>

#include "work.h"

namespace rex::graphics::gta4_webgpu {

// One decoded subresource in host layout (rows of `row_pitch` bytes).
struct DecodedSlice {
  uint32_t level = 0;
  uint32_t layer = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;
  uint32_t row_pitch = 0;
  uint32_t rows = 0;  // Rows of blocks per image.
  std::vector<uint8_t> bytes;
};

// Host storage format of a guest texture format: CTX1/DXN expand to RG8,
// DXT5A to R8 and DXT3A to DXT3, as in the Metal and Vulkan renderers.
const FormatInfo* HostTextureFormatInfo(const TextureInfo& info);
bool HostTextureExpanded(const TextureInfo& info);

// Untiles and converts every captured level and layer of a texture.
bool DecodeTexture(const TextureInfo& info, const TextureCapture& capture,
                   std::vector<DecodedSlice>& slices, std::string& error);

}  // namespace rex::graphics::gta4_webgpu
