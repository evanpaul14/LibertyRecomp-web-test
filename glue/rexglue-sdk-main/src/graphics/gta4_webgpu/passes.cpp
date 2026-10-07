#include "renderer_state.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "../gta4_native/native_color_output.h"
#include "../gta4_native/native_texture_image_identity.h"
#include "canvas.h"

REXCVAR_DEFINE_STRING(webgpu_frame_dump_path, "", "GPU/Diagnostics",
                      "Web build: write presented frames as PAM images with this path prefix");
REXCVAR_DEFINE_UINT32(webgpu_frame_dump_interval, 60, "GPU/Diagnostics",
                      "Web build: dump every Nth presented frame");

namespace rex::graphics::gta4_webgpu {
namespace {
using namespace gta4_native;

constexpr uint32_t kDeviceCompletedFrame = 16552;

uint32_t TexelBytes(wgpu::TextureFormat format) {
  switch (format) {
    case wgpu::TextureFormat::R8Unorm: return 1;
    case wgpu::TextureFormat::RG8Unorm: return 2;
    case wgpu::TextureFormat::RGBA8Unorm:
    case wgpu::TextureFormat::RG16Float:
    case wgpu::TextureFormat::R32Float: return 4;
    case wgpu::TextureFormat::RGBA16Float: return 8;
    default: return 0;
  }
}

wgpu::TextureView Subresource(const TextureResource& texture, uint32_t level, uint32_t slice,
                              uint32_t& depth_slice) {
  wgpu::TextureViewDescriptor view{};
  view.baseMipLevel = level;
  view.mipLevelCount = 1;
  depth_slice = wgpu::kDepthSliceUndefined;
  if (texture.dimension == wgpu::TextureViewDimension::e3D) {
    view.dimension = wgpu::TextureViewDimension::e3D;
    depth_slice = slice;
  } else {
    view.dimension = wgpu::TextureViewDimension::e2D;
    view.baseArrayLayer = slice;
    view.arrayLayerCount = 1;
  }
  return texture.texture.CreateView(&view);
}

wgpu::TextureView DepthView(const SurfaceResource& surface) {
  wgpu::TextureViewDescriptor view{};
  view.aspect = wgpu::TextureAspect::DepthOnly;
  return surface.texture.CreateView(&view);
}
}  // namespace

void Renderer::State::PresentToCanvas(const TextureResource& source, std::string& error) {
  uint32_t width = 0, height = 0;
  if (!PollCanvas(width, height)) return;
  if (!surface) {
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas{};
    canvas.selector = kCanvasSelector;
    wgpu::SurfaceDescriptor descriptor{};
    descriptor.nextInChain = &canvas;
    surface = instance.CreateSurface(&descriptor);
    wgpu::SurfaceCapabilities capabilities{};
    surface.GetCapabilities(adapter, &capabilities);
    surface_format = capabilities.formatCount ? capabilities.formats[0]
                                              : wgpu::TextureFormat::BGRA8Unorm;
    REXLOG_INFO("gta4-webgpu: presenting to the page canvas ({})", uint32_t(surface_format));
  }
  if (width != surface_width || height != surface_height) {
    wgpu::SurfaceConfiguration configuration{};
    configuration.device = device;
    configuration.format = surface_format;
    configuration.usage = wgpu::TextureUsage::RenderAttachment;
    configuration.width = width;
    configuration.height = height;
    configuration.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    configuration.presentMode = wgpu::PresentMode::Fifo;
    surface.Configure(&configuration);
    surface_width = width;
    surface_height = height;
  }
  wgpu::SurfaceTexture current{};
  surface.GetCurrentTexture(&current);
  if (!current.texture) return;
  wgpu::TextureViewDescriptor view{};
  view.dimension = wgpu::TextureViewDimension::e2D;
  view.arrayLayerCount = 1;
  view.mipLevelCount = 1;
  const std::array<float, 12> parameters{0, 0, 0, 0, 0, 0, float(width), float(height), 0, 0, 0, 0};
  // The browser shows the canvas when the render worker next yields.
  UtilityPass("present", current.texture.CreateView(), wgpu::kDepthSliceUndefined, false,
              surface_format, width, height, {0, 0, int32_t(width), int32_t(height)},
              source.texture.CreateView(&view), parameters, false, error);
}

bool Renderer::State::Resolve(const Work& work, std::string& error) {
  const auto c = work.As<ResolveCommand>();
  ++stats.resolves;
  const uint32_t flags = NormalizeResolveSampleFlags(c.flags, c.source.sample_type);
  const bool depth = (flags & 7u) == 4;
  if ((flags & 7u) > 4) {
    error = "Invalid resolve source attachment";
    return false;
  }
  auto source = ResolveSource(c.source, depth, error);
  if (!source) return false;
  const auto fetch = std::bit_cast<xenos::xe_gpu_texture_fetch_t>(c.destination_fetch);
  std::shared_ptr<TextureResource> destination;
  if (auto found = textures.find(c.destination_texture);
      found != textures.end() && found->second->gpu_produced &&
      NativeTextureImageFetchEqual(found->second->fetch, fetch)) {
    destination = found->second;
  } else {
    destination = CreateTexture(fetch, true, error);
    if (!destination) return false;
    textures[c.destination_texture] = destination;
  }
  if (destination->depth_values != depth) {
    error = "Resolve color/depth aspect mismatch";
    return false;
  }
  const uint32_t level = c.destination_level, slice = c.destination_slice_or_face;
  if (level >= destination->mip_levels ||
      (destination->dimension != wgpu::TextureViewDimension::e3D && slice >= destination->layers)) {
    error = "Resolve destination subresource is out of range";
    return false;
  }
  const uint32_t target_w = std::max(1u, destination->width >> level);
  const uint32_t target_h = std::max(1u, destination->height >> level);
  const int32_t dx = c.destination_point_valid ? c.destination_point.x : 0;
  const int32_t dy = c.destination_point_valid ? c.destination_point.y : 0;
  if (dx < 0 || dy < 0 || uint32_t(dx) >= target_w || uint32_t(dy) >= target_h) {
    error = "Resolve destination origin is outside the image";
    return false;
  }
  auto rect = c.source_rectangle_valid
                  ? c.source_rectangle
                  : ResolveRectangle{0, 0, int32_t(source->width), int32_t(source->height)};
  rect.left = std::clamp(rect.left, 0, int32_t(source->width));
  rect.top = std::clamp(rect.top, 0, int32_t(source->height));
  rect.right = std::clamp(rect.right, rect.left, int32_t(source->width));
  rect.bottom = std::clamp(rect.bottom, rect.top, int32_t(source->height));
  const uint32_t copy_w = std::min(uint32_t(rect.right - rect.left), target_w - uint32_t(dx));
  const uint32_t copy_h = std::min(uint32_t(rect.bottom - rect.top), target_h - uint32_t(dy));
  if (trace)
    REXLOG_INFO("webgpu-trace: resolve flags={:08X} source={:08X} chosen={:08X} serial={} "
                "destination={:08X} {}x{} rect={},{},{},{} copy={}x{} at {},{}",
                flags, c.source.handle, source->descriptor.handle, source->content_serial,
                c.destination_texture, destination->width, destination->height, rect.left,
                rect.top, rect.right, rect.bottom, copy_w, copy_h, dx, dy);
  EndPass();
  if (!Begin(error)) return false;
  if (copy_w && copy_h) {
    const int32_t exponent = depth ? 0 : NativeResolveExponent(flags);
    if (!depth && !exponent && source->format == destination->format) {
      wgpu::TexelCopyTextureInfo from{};
      from.texture = source->texture;
      from.origin = {uint32_t(rect.left), uint32_t(rect.top), 0};
      wgpu::TexelCopyTextureInfo to{};
      to.texture = destination->texture;
      to.mipLevel = level;
      to.origin = {uint32_t(dx), uint32_t(dy), slice};
      wgpu::Extent3D size{copy_w, copy_h, 1};
      encoder.CopyTextureToTexture(&from, &to, &size);
    } else {
      uint32_t depth_slice = 0;
      auto target = Subresource(*destination, level, slice, depth_slice);
      const float scale = std::ldexp(1.0f, exponent);
      const std::array<float, 12> parameters{
          float(rect.left), float(rect.top), float(dx),     float(dy),
          float(copy_w),    float(copy_h),   float(copy_w), float(copy_h),
          scale,            0,               0,             0};
      if (!UtilityPass(depth ? "copy_depth_to_color" : "copy_color", target, depth_slice, false,
                       destination->format, target_w, target_h,
                       {dx, dy, dx + int32_t(copy_w), dy + int32_t(copy_h)},
                       depth ? DepthView(*source) : source->view, parameters, true, error))
        return false;
    }
    destination->content_serial = ++content_serial;
  }
  // Clears that ride along with the resolve.
  if (flags & 0x300u) {
    std::array<float, 4> color{};
    for (size_t i = 0; i < color.size(); ++i) color[i] = std::bit_cast<float>(c.clear_color_bits[i]);
    const float clear_depth = float(std::bit_cast<double>(c.clear_depth_bits));
    if (flags & 0x100u) {
      const uint32_t index = flags & 7u;
      const auto& descriptor =
          index < kRenderTargetCount && work.colors[index].handle ? work.colors[index] : c.source;
      auto surface = Surface(descriptor, false, error);
      const ResolveRectangle all{0, 0, int32_t(descriptor.width), int32_t(descriptor.height)};
      if (!surface || !ClearSurface(surface, 1u, c.source_rectangle_valid ? c.source_rectangle : all,
                                    color, clear_depth, c.clear_stencil, error))
        return false;
    }
    if ((flags & 0x200u) && work.depth.handle) {
      auto surface = Surface(work.depth, true, error);
      const ResolveRectangle all{0, 0, int32_t(work.depth.width), int32_t(work.depth.height)};
      if (!surface || !ClearSurface(surface, 6u, c.source_rectangle_valid ? c.source_rectangle : all,
                                    color, clear_depth, c.clear_stencil, error))
        return false;
    }
  }
  return true;
}

bool Renderer::State::Handoff(const Work& work, std::string& error) {
  const auto c = work.As<DepthSurfaceHandoffCommand>();
  auto source = ResolveSource(c.source, true, error);
  auto destination = Surface(c.destination, true, error);
  if (!source || !destination) return false;
  if (!source->initialized || source == destination) return true;
  if (source->width != destination->width || source->height != destination->height) {
    error = "Depth handoff between different extents";
    return false;
  }
  // Depth only; the destination keeps its stencil (WebGPU cannot export
  // stencil from a shader, so the rebuild policy is not reproduced).
  const std::array<float, 12> parameters{0, 0, 0, 0,
                                         float(source->width), float(source->height),
                                         float(source->width), float(source->height),
                                         0, 0, 0, 0};
  if (!UtilityPass("copy_depth", destination->view, wgpu::kDepthSliceUndefined, true,
                   destination->format, destination->width, destination->height,
                   {0, 0, int32_t(destination->width), int32_t(destination->height)},
                   DepthView(*source), parameters, destination->initialized, error))
    return false;
  destination->initialized = true;
  destination->content_serial = ++content_serial;
  return true;
}

Renderer::Status Renderer::State::Present(const Work& work, std::string& error) {
  const auto c = work.As<PresentCommand>();
  std::shared_ptr<TextureResource> source;
  if (auto found = textures.find(c.frontbuffer_texture); found != textures.end() &&
                                                          found->second->gpu_produced) {
    source = found->second;
  } else if (work.present_source) {
    source = SampledTexture(c.frontbuffer_texture, work.present_source->fetch, work.present_source,
                            error);
  }
  const auto acknowledge = [&] {
    if (c.device && uint64_t(c.device) + kDeviceCompletedFrame + 4 <= 0x100000000ull) {
      const uint32_t completed = __builtin_bswap32(c.submitted_frame);
      std::memcpy(memory->TranslateVirtual<uint8_t*>(c.device + kDeviceCompletedFrame),
                  &completed, sizeof(completed));
    }
  };
  if (c.submitted_frame <= 3 || c.submitted_frame % 60 == 0)
    REXLOG_INFO("gta4-webgpu: frame={} draws={} no-targets={} empty-viewport={} empty-scissor={} "
                "failed={} clears={} resolves={} frontbuffer={:08X} source={} {}x{} "
                "pipelines={} textures={} surfaces={}",
                c.submitted_frame, stats.draws, stats.no_targets, stats.empty_viewport,
                stats.empty_scissor, stats.failed, stats.clears, stats.resolves,
                c.frontbuffer_texture, !source ? "none" : source->gpu_produced ? "gpu" : "cpu",
                source ? source->width : 0, source ? source->height : 0, pipelines.size(),
                textures.size(), surfaces.size());
  if (c.submitted_frame <= 3 || c.submitted_frame % 60 == 0) {
    std::string counts;
    for (size_t i = 0; i < stats.commands.size(); ++i)
      if (stats.commands[i]) counts += fmt::format(" {}:{}", i, stats.commands[i]);
    REXLOG_INFO("gta4-webgpu: frame={} commands{}", c.submitted_frame, counts);
  }
  stats = {};
  frame_draws = 0;

  if (source) PresentToCanvas(*source, error);
  const std::string dump_path = REXCVAR_GET(webgpu_frame_dump_path);
  const uint32_t interval = std::max(1u, REXCVAR_GET(webgpu_frame_dump_interval));
  const bool dump = !dump_path.empty() && source && c.width && c.height &&
                    c.submitted_frame % interval == 0;
  if (!dump) {
    Flush(error);
    acknowledge();
    BeginFrame();
    if (!source && error.empty()) error = "Title frontbuffer is not a ready color image";
    return Status::kYield;
  }

  // Render the frontbuffer at its presentation size, then read it back.
  const uint32_t width = c.width, height = c.height;
  wgpu::TextureDescriptor descriptor{};
  descriptor.size = {width, height, 1};
  descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
  descriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  auto image = device.CreateTexture(&descriptor);
  wgpu::TextureViewDescriptor view{};
  view.dimension = wgpu::TextureViewDimension::e2D;
  view.arrayLayerCount = 1;
  view.mipLevelCount = 1;
  const std::array<float, 12> parameters{0, 0, 0, 0, 0, 0, float(width), float(height), 0, 0, 0, 0};
  if (!UtilityPass("present", image.CreateView(), wgpu::kDepthSliceUndefined, false,
                   descriptor.format, width, height,
                   {0, 0, int32_t(width), int32_t(height)}, source->texture.CreateView(&view),
                   parameters, false, error)) {
    Flush(error);
    acknowledge();
    BeginFrame();
    return Status::kYield;
  }
  const uint32_t row = (width * 4 + 255) & ~255u;
  wgpu::BufferDescriptor buffer_descriptor{};
  buffer_descriptor.size = uint64_t(row) * height;
  buffer_descriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  auto buffer = device.CreateBuffer(&buffer_descriptor);
  wgpu::TexelCopyTextureInfo from{};
  from.texture = image;
  wgpu::TexelCopyBufferInfo to{};
  to.buffer = buffer;
  to.layout.bytesPerRow = row;
  to.layout.rowsPerImage = height;
  wgpu::Extent3D size{width, height, 1};
  encoder.CopyTextureToBuffer(&from, &to, &size);
  Flush(error);
  acknowledge();
  BeginFrame();
  const std::string path = fmt::format("{}_{:06}.pam", dump_path, c.submitted_frame);
  const uint64_t bytes = buffer_descriptor.size;
  buffer.MapAsync(
      wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::AllowSpontaneous,
      [this, buffer, path, width, height, row](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (status == wgpu::MapAsyncStatus::Success) {
          const auto* pixels = static_cast<const uint8_t*>(buffer.GetConstMappedRange());
          if (FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fprintf(file, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                         width, height);
            for (uint32_t y = 0; y < height; ++y) std::fwrite(pixels + size_t(y) * row, 1, width * 4, file);
            std::fclose(file);
            REXLOG_INFO("gta4-webgpu: wrote {}", path);
          }
        }
        buffer.Unmap();
        if (resume) resume();
      });
  return Status::kPending;
}

Renderer::Status Renderer::State::Readback(const Work& work, std::string& error) {
  const auto lock = work.As<TextureLockCommand>();
  TextureLockResult result{};
  const auto finish = [&](bool copied) {
    result.copied_to_guest = copied;
    work.execute->result.resize(sizeof(result));
    std::memcpy(work.execute->result.data(), &result, sizeof(result));
  };
  const auto found = textures.find(lock.texture);
  if (found == textures.end()) {
    finish(false);  // No host-produced contents exist for a CPU texture.
    return Status::kDone;
  }
  const auto texture = found->second;
  result.generation = texture->content_serial;
  if (!texture->gpu_produced) {
    finish(false);
    return Status::kDone;
  }
  const auto& info = texture->info;
  const uint32_t layers = info.dimension == xenos::DataDimension::kCube ? 6u
                          : info.is_stacked ? info.depth + 1
                                            : 1u;
  const auto* format = info.format_info();
  const uint32_t host_bytes = TexelBytes(texture->format);
  if (info.dimension == xenos::DataDimension::k3D || lock.dimension != TextureLockDimension::k2D ||
      lock.array_index >= layers || lock.level >= texture->mip_levels || !format ||
      format->block_width != 1 || format->block_height != 1 || !host_bytes ||
      !std::has_single_bit(format->bytes_per_block())) {
    error = "Unsupported texture readback";
    finish(false);
    return Status::kDone;
  }
  const uint32_t block_bytes = format->bytes_per_block();
  uint32_t width = 0, height = 0, packed_x = 0, packed_y = 0;
  info.GetMipSize(lock.level, &width, &height);
  width = std::min(width, std::max(1u, texture->width >> lock.level));
  height = std::min(height, std::max(1u, texture->height >> lock.level));
  const auto layout = texture_util::GetGuestTextureLayout(
      info.dimension, info.pitch >> 5, info.width + 1, info.height + 1, info.depth + 1,
      info.is_tiled, info.format, info.has_packed_mips, info.memory.base_address != 0,
      info.mip_max_level);
  const auto& guest_level = lock.level == 0 ? layout.base : layout.mips[lock.level];
  const uint64_t base_address = uint64_t(info.GetMipLocation(lock.level, &packed_x, &packed_y, true)) +
                                uint64_t(lock.array_index) * guest_level.array_slice_stride_bytes;
  const auto extent = info.GetMipExtent(lock.level, true);
  const bool tiled = info.is_tiled;
  const uint32_t pitch = extent.block_pitch_h;
  const auto endianness = info.endianness;
  const bool depth_values = texture->depth_values;
  const bool float_depth = GetBaseFormat(info.format) == xenos::TextureFormat::k_24_8_FLOAT;

  EndPass();
  if (!Begin(error)) {
    finish(false);
    return Status::kDone;
  }
  const uint32_t row = (width * host_bytes + 255) & ~255u;
  wgpu::BufferDescriptor descriptor{};
  descriptor.size = uint64_t(row) * height;
  descriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  auto buffer = device.CreateBuffer(&descriptor);
  wgpu::TexelCopyTextureInfo from{};
  from.texture = texture->texture;
  from.mipLevel = lock.level;
  from.origin = {0, 0, lock.array_index};
  wgpu::TexelCopyBufferInfo to{};
  to.buffer = buffer;
  to.layout.bytesPerRow = row;
  to.layout.rowsPerImage = height;
  wgpu::Extent3D size{width, height, 1};
  encoder.CopyTextureToBuffer(&from, &to, &size);
  Flush(error);
  auto slot = work.execute;
  auto* guest_memory = memory;
  buffer.MapAsync(
      wgpu::MapMode::Read, 0, descriptor.size, wgpu::CallbackMode::AllowSpontaneous,
      [this, buffer, slot, result, guest_memory, width, height, row, host_bytes, block_bytes,
       packed_x, packed_y, base_address, tiled, pitch, endianness, depth_values,
       float_depth](wgpu::MapAsyncStatus status, wgpu::StringView message) mutable {
        bool ok = status == wgpu::MapAsyncStatus::Success;
        if (ok) {
          const auto* bytes = static_cast<const uint8_t*>(buffer.GetConstMappedRange());
          for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
              const int64_t offset =
                  tiled ? texture_util::GetTiledOffset2D(packed_x + x, packed_y + y, pitch,
                                                         std::countr_zero(block_bytes))
                        : int64_t((uint64_t(packed_y + y) * pitch + packed_x + x) * block_bytes);
              if (offset < 0 || base_address + uint64_t(offset) + block_bytes > 0x20000000ull)
                continue;
              const uint8_t* texel = bytes + size_t(y) * row + size_t(x) * host_bytes;
              uint32_t packed = 0;
              if (depth_values) {
                float value;
                std::memcpy(&value, texel, sizeof(value));
                const uint32_t quantized =
                    float_depth ? xenos::Float32To20e4(value, false)
                                : uint32_t(std::nearbyint(std::clamp(double(value), 0.0, 1.0) *
                                                          16777215.0));
                packed = quantized << 8;
                texel = reinterpret_cast<const uint8_t*>(&packed);
              }
              texture_conversion::CopySwapBlock(
                  endianness,
                  guest_memory->TranslatePhysical<uint8_t*>(uint32_t(base_address + offset)),
                  texel, block_bytes);
            }
          }
        } else {
          slot->error = fmt::format("texture readback map failed: {}",
                                    std::string_view(message.data ? message.data : "",
                                                     message.data ? message.length : 0));
        }
        buffer.Unmap();
        result.copied_to_guest = ok;
        slot->result.resize(sizeof(result));
        std::memcpy(slot->result.data(), &result, sizeof(result));
        slot->Finish(ok);
        if (resume) resume();
      });
  return Status::kPending;
}

}  // namespace rex::graphics::gta4_webgpu
