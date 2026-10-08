#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/graphics/gta4_native/title_commands.h>

namespace rex::graphics::gta4_webgpu {

// One translated title shader (tools/webgpu/spirv_to_wgsl.py). Constants are
// read from one uniform buffer (vertex at 0, pixel at 4096, SharedConstants at
// 8192, the specialization word at 8192 + 0x500). Texture slot s binds at
// @group(1) @binding(s), its sampler at @binding(32 + s).
struct ShaderRecord {
  uint64_t hash = 0;
  gta4_native::ShaderStage stage = gta4_native::ShaderStage::kPixel;
  uint32_t texture_mask = 0;
  uint32_t cube_mask = 0;
  uint32_t sampler_mask = 0;
  uint32_t specialization_mask = 0;
  uint32_t color_output_mask = 0;
  // WGSL @location(i) of a vertex shader reads semantic attributes[i].
  std::vector<uint8_t> attributes;
  std::string_view early;
  std::string_view late;  // Alpha-test/alpha-to-mask variant; may be empty.
};

// Group 0 holds the title's constants as three uniform bindings, each with
// its own dynamic offset: vertex constants (256 registers), pixel constants
// (256 registers, of which the title sets 224) and the shared constants with
// the specialization word in their last register.
inline constexpr uint32_t kUniformVertexBytes = 4096;
inline constexpr uint32_t kUniformPixelBytes = 4096;
inline constexpr uint32_t kUniformSharedBytes = 81 * 16;
inline constexpr uint32_t kUniformSpecializationOffset = 0x500;  // In the shared binding.
inline constexpr uint32_t kUniformBindings = 3;
inline constexpr uint32_t kSamplerBindingBase = 32;

class ShaderArchive {
 public:
  // Decompresses and indexes the archive embedded in the executable.
  bool Load(std::string& error);
  const ShaderRecord* Find(uint64_t hash, gta4_native::ShaderStage stage) const;
  size_t size() const { return records_[0].size() + records_[1].size(); }

 private:
  bool Parse(std::span<const uint8_t> bytes, std::string& error);
  std::vector<uint8_t> storage_;
  std::unordered_map<uint64_t, ShaderRecord> records_[2];  // By ShaderStage.
};

// Process-wide archive shared by the producer threads and the render thread.
// Immutable after the first successful load.
const ShaderArchive* GetShaderArchive(std::string& error);

}  // namespace rex::graphics::gta4_webgpu
