#include "texture_decode.h"

#include <bit>
#include <cstring>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>

namespace rex::graphics::gta4_webgpu {

const FormatInfo* HostTextureFormatInfo(const TextureInfo& info) {
  const auto base = GetBaseFormat(info.format);
  return FormatInfo::Get(
      base == xenos::TextureFormat::k_DXT3A ? xenos::TextureFormat::k_DXT2_3
      : (base == xenos::TextureFormat::k_DXN || base == xenos::TextureFormat::k_CTX1)
          ? xenos::TextureFormat::k_8_8
      : base == xenos::TextureFormat::k_DXT5A ? xenos::TextureFormat::k_8
                                               : base);
}

bool HostTextureExpanded(const TextureInfo& info) {
  const auto base = GetBaseFormat(info.format);
  return base == xenos::TextureFormat::k_CTX1 || base == xenos::TextureFormat::k_DXN ||
         base == xenos::TextureFormat::k_DXT5A;
}

bool DecodeTexture(const TextureInfo& info, const TextureCapture& capture,
                   std::vector<DecodedSlice>& slices, std::string& error) {
  const auto base = GetBaseFormat(info.format);
  const auto* guest = info.format_info();
  const auto* host = HostTextureFormatInfo(info);
  if (!guest || !host || !std::has_single_bit(guest->bytes_per_block()) ||
      !host->bytes_per_block()) {
    error = "Unsupported texture block shape";
    return false;
  }
  const uint32_t guest_block = guest->bytes_per_block();
  const uint32_t host_block = host->bytes_per_block();
  const bool volume = info.dimension == xenos::DataDimension::k3D;
  const bool expand = HostTextureExpanded(info);
  const uint32_t layers = volume ? 1
                          : info.dimension == xenos::DataDimension::kCube ? 6
                          : info.is_stacked ? info.depth + 1
                                            : 1;
  const auto layout = texture_util::GetGuestTextureLayout(
      info.dimension, info.pitch >> 5, info.width + 1, info.height + 1, info.depth + 1,
      info.is_tiled, info.format, info.has_packed_mips, info.memory.base_address != 0,
      info.mip_max_level);
  for (const auto& mip : capture.mips) {
    uint32_t width = 0, height = 0, packed_x = 0, packed_y = 0;
    info.GetMipSize(mip.level, &width, &height);
    const uint32_t depth = volume ? std::max(1u, (info.depth + 1) >> mip.level) : 1;
    const auto extent = info.GetMipExtent(mip.level, true);
    info.GetMipLocation(mip.level, &packed_x, &packed_y, true);
    const auto& guest_level = mip.level == 0 ? layout.base : layout.mips[mip.level];
    const uint32_t blocks_x = (width + guest->block_width - 1) / guest->block_width;
    const uint32_t blocks_y = (height + guest->block_height - 1) / guest->block_height;
    const uint32_t storage_width = expand ? blocks_x * guest->block_width : width;
    const uint32_t storage_height = expand ? blocks_y * guest->block_height : height;
    const uint32_t row_pitch =
        ((storage_width + host->block_width - 1) / host->block_width) * host_block;
    const uint32_t rows = (storage_height + host->block_height - 1) / host->block_height;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      DecodedSlice slice;
      slice.level = mip.level;
      slice.layer = layer;
      slice.width = width;
      slice.height = height;
      slice.depth = depth;
      slice.row_pitch = row_pitch;
      slice.rows = rows;
      slice.bytes.assign(size_t(row_pitch) * rows * depth, 0);
      for (uint32_t z = 0; z < depth; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          for (uint32_t x = 0; x < blocks_x; ++x) {
            const uint32_t sx = packed_x + x, sy = packed_y + y;
            const int64_t offset =
                info.is_tiled
                    ? (volume ? texture_util::GetTiledOffset3D(sx, sy, z, extent.block_pitch_h,
                                                               extent.block_pitch_v,
                                                               std::countr_zero(guest_block))
                              : texture_util::GetTiledOffset2D(sx, sy, extent.block_pitch_h,
                                                               std::countr_zero(guest_block)))
                    : int64_t(((uint64_t(z) * extent.block_pitch_v + sy) * extent.block_pitch_h +
                               sx) *
                              guest_block);
            const uint64_t source_offset =
                uint64_t(layer) * guest_level.array_slice_stride_bytes + uint64_t(offset);
            if (offset < 0 || source_offset > mip.bytes.size() ||
                guest_block > mip.bytes.size() - source_offset) {
              error = "Guest texture block is outside its captured level";
              return false;
            }
            const uint8_t* input = mip.bytes.data() + source_offset;
            uint8_t* output =
                slice.bytes.data() + size_t(z) * row_pitch * rows +
                (expand ? size_t(y) * guest->block_height * row_pitch +
                              size_t(x) * guest->block_width * host_block
                        : size_t(y) * row_pitch + size_t(x) * host_block);
            switch (base) {
              case xenos::TextureFormat::k_CTX1:
                texture_conversion::ConvertTexelCTX1ToR8G8(info.endianness, output, input,
                                                           row_pitch);
                break;
              case xenos::TextureFormat::k_DXN:
                texture_conversion::ConvertTexelDXNToR8G8(info.endianness, output, input,
                                                          row_pitch);
                break;
              case xenos::TextureFormat::k_DXT5A:
                texture_conversion::ConvertTexelDXT5AToR8(info.endianness, output, input,
                                                          row_pitch);
                break;
              case xenos::TextureFormat::k_DXT3A:
                texture_conversion::ConvertTexelDXT3AToDXT3(info.endianness, output, input,
                                                            host_block);
                break;
              default:
                texture_conversion::CopySwapBlock(info.endianness, output, input, host_block);
                break;
            }
          }
        }
      }
      slices.push_back(std::move(slice));
    }
  }
  return true;
}

}  // namespace rex::graphics::gta4_webgpu
