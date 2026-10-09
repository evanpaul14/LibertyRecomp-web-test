#include "renderer_state.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <fmt/format.h>

#include <rex/graphics/pipeline/texture/util.h>

#include "texture_decode.h"
#include "vertex_decode.h"

namespace rex::graphics::gta4_webgpu {
namespace {
using namespace gta4_native;

constexpr uint64_t kCacheRetainFrames = 300;
// Bind groups keep their textures alive, so unused ones go sooner.
constexpr uint64_t kGroupRetainFrames = 30;

wgpu::AddressMode Address(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat: return wgpu::AddressMode::Repeat;
    case xenos::ClampMode::kMirroredRepeat:
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder: return wgpu::AddressMode::MirrorRepeat;
    default: return wgpu::AddressMode::ClampToEdge;  // WebGPU has no border color.
  }
}

uint32_t MaxMipLevels(uint32_t width, uint32_t height, uint32_t depth) {
  return uint32_t(std::bit_width(std::max({width, height, depth})));
}
}  // namespace

wgpu::TextureFormat SampledFormat(xenos::TextureFormat format) {
  switch (GetBaseFormat(format)) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_DXT5A: return wgpu::TextureFormat::R8Unorm;
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_CTX1: return wgpu::TextureFormat::RG8Unorm;
    case xenos::TextureFormat::k_8_8_8_8: return wgpu::TextureFormat::RGBA8Unorm;
    case xenos::TextureFormat::k_DXT1: return wgpu::TextureFormat::BC1RGBAUnorm;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT3A: return wgpu::TextureFormat::BC2RGBAUnorm;
    case xenos::TextureFormat::k_DXT4_5: return wgpu::TextureFormat::BC3RGBAUnorm;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT: return wgpu::TextureFormat::RGBA16Float;
    case xenos::TextureFormat::k_16_16_FLOAT: return wgpu::TextureFormat::RG16Float;
    case xenos::TextureFormat::k_32_FLOAT: return wgpu::TextureFormat::R32Float;
    // Resolved depth is stored as float values so every title shader can
    // sample it with a filtering sampler; green keeps the stencil for packed
    // depth aliases and readback.
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT: return wgpu::TextureFormat::RG32Float;
    default: return wgpu::TextureFormat::Undefined;
  }
}

wgpu::TextureFormat SurfaceFormat(uint32_t format, bool depth, wgpu::TextureFormat depth_format) {
  if (depth) return format == 0x1A220197u ? depth_format : wgpu::TextureFormat::Undefined;
  switch (format) {
    case 0x18280186u: return wgpu::TextureFormat::RGBA8Unorm;
    case 0x1A2201BFu: return wgpu::TextureFormat::RGBA16Float;
    case 0x2DA2ABA4u: return wgpu::TextureFormat::R32Float;
    case 0x2D22AB9Fu:
    case 0x2D20AB8Du: return wgpu::TextureFormat::RG16Float;
    default: return wgpu::TextureFormat::Undefined;
  }
}

bool IsCompressed(wgpu::TextureFormat format) {
  return format == wgpu::TextureFormat::BC1RGBAUnorm || format == wgpu::TextureFormat::BC2RGBAUnorm ||
         format == wgpu::TextureFormat::BC3RGBAUnorm;
}

std::shared_ptr<SurfaceResource> Renderer::State::Surface(const SurfaceDescriptor& descriptor,
                                                          bool depth, std::string& error) {
  if (!descriptor.handle || !descriptor.width || !descriptor.height ||
      descriptor.width > limits.maxTextureDimension2D ||
      descriptor.height > limits.maxTextureDimension2D) {
    error = "Invalid surface descriptor";
    return {};
  }
  const auto format = SurfaceFormat(descriptor.format, depth, depth_format);
  if (format == wgpu::TextureFormat::Undefined) {
    error = fmt::format("Unsupported surface format {:08X}", descriptor.format);
    return {};
  }
  if (auto found = surfaces.find(descriptor.handle); found != surfaces.end()) {
    auto& surface = found->second;
    if (surface->depth == depth && surface->format == format &&
        surface->width == descriptor.width && surface->height == descriptor.height) {
      surface->descriptor = descriptor;
      return surface;
    }
  }
  auto surface = std::make_shared<SurfaceResource>();
  wgpu::TextureDescriptor texture{};
  texture.size = {descriptor.width, descriptor.height, 1};
  texture.format = format;
  texture.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                  wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
  const std::string label = fmt::format("surface {:08X}", descriptor.handle);
  texture.label = wgpu::StringView(label.data(), label.size());
  surface->texture = device.CreateTexture(&texture);
  surface->view = surface->texture.CreateView();
  surface->format = format;
  surface->descriptor = descriptor;
  surface->depth = depth;
  surface->width = descriptor.width;
  surface->height = descriptor.height;
  surfaces[descriptor.handle] = surface;
  GuestSurfaceView view{};
  if (!depth && DecodeGuestSurfaceView(descriptor, false, view)) {
    auto& handles = color_views[GetGuestPlacementKey(view)];
    if (std::find(handles.begin(), handles.end(), descriptor.handle) == handles.end())
      handles.push_back(descriptor.handle);
  }
  return surface;
}

std::shared_ptr<SurfaceResource> Renderer::State::ResolveSource(
    const SurfaceDescriptor& descriptor, bool depth, std::string& error) {
  if (!depth) {
    // Another surface handle may alias the same EDRAM placement; resolve the
    // most recently produced one, as the Metal and Vulkan renderers do.
    GuestSurfaceView view{};
    if (DecodeGuestSurfaceView(descriptor, false, view)) {
      const auto group = color_views.find(GetGuestPlacementKey(view));
      if (group != color_views.end()) {
        std::shared_ptr<SurfaceResource> latest;
        for (uint32_t handle : group->second) {
          const auto found = surfaces.find(handle);
          if (found == surfaces.end() || found->second->depth || !found->second->content_serial)
            continue;
          if (!latest || found->second->content_serial > latest->content_serial)
            latest = found->second;
        }
        if (latest) return latest;
      }
    }
  } else if (auto found = surfaces.find(descriptor.handle); found != surfaces.end()) {
    return found->second;
  }
  return Surface(descriptor, depth, error);
}

std::shared_ptr<TextureResource> Renderer::State::CreateTexture(
    const xenos::xe_gpu_texture_fetch_t& fetch, bool gpu_produced, std::string& error) {
  TextureInfo info{};
  if (!TextureInfo::Prepare(fetch, &info) || info.mip_min_level > info.mip_max_level ||
      info.mip_max_level >= xenos::kTextureMaxMips) {
    error = "Invalid guest texture descriptor";
    return {};
  }
  const auto format = SampledFormat(info.format);
  if (format == wgpu::TextureFormat::Undefined) {
    error = fmt::format("Unsupported texture format {}", uint32_t(info.format));
    return {};
  }
  const bool compressed = IsCompressed(format);
  if (compressed && (!bc_textures || gpu_produced)) {
    error = "BC texture compression is unavailable";
    return {};
  }
  auto resource = std::make_shared<TextureResource>();
  uint32_t width = info.width + 1, height = info.height + 1, layers = 1, depth = 1;
  wgpu::TextureDimension dimension = wgpu::TextureDimension::e2D;
  switch (info.dimension) {
    case xenos::DataDimension::k2DOrStacked:
      if (info.is_stacked) {
        layers = info.depth + 1;
        resource->dimension = wgpu::TextureViewDimension::e2DArray;
      }
      break;
    case xenos::DataDimension::kCube:
      if (width != height) {
        error = "Non-square cube texture";
        return {};
      }
      layers = 6;
      resource->dimension = wgpu::TextureViewDimension::Cube;
      break;
    case xenos::DataDimension::k3D:
      depth = info.depth + 1;
      dimension = wgpu::TextureDimension::e3D;
      resource->dimension = wgpu::TextureViewDimension::e3D;
      break;
    default:
      error = "Unsupported texture dimension";
      return {};
  }
  if (compressed) {
    width = (width + 3) & ~3u;
    height = (height + 3) & ~3u;
  }
  if (width > limits.maxTextureDimension2D || height > limits.maxTextureDimension2D) {
    error = "Texture exceeds the device limits";
    return {};
  }
  const uint32_t mip_levels =
      std::min(info.mip_max_level + 1, MaxMipLevels(width, height, depth));
  wgpu::TextureDescriptor descriptor{};
  descriptor.dimension = dimension;
  descriptor.size = {width, height, dimension == wgpu::TextureDimension::e3D ? depth : layers};
  descriptor.format = format;
  descriptor.mipLevelCount = mip_levels;
  descriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst |
                     wgpu::TextureUsage::CopySrc;
  if (gpu_produced) descriptor.usage |= wgpu::TextureUsage::RenderAttachment;
  resource->texture = device.CreateTexture(&descriptor);
  wgpu::TextureViewDescriptor view{};
  view.dimension = resource->dimension;
  resource->view = resource->texture.CreateView(&view);
  resource->format = format;
  resource->info = info;
  resource->fetch = fetch;
  resource->gpu_produced = gpu_produced;
  resource->depth_values = GetBaseFormat(info.format) == xenos::TextureFormat::k_24_8 ||
                           GetBaseFormat(info.format) == xenos::TextureFormat::k_24_8_FLOAT;
  resource->width = width;
  resource->height = height;
  resource->layers = dimension == wgpu::TextureDimension::e3D ? depth : layers;
  resource->mip_levels = mip_levels;
  return resource;
}

bool Renderer::State::Upload(TextureResource& texture, const TextureCapture& capture,
                             std::string& error) {
  if (texture.depth_values) {
    error = "CPU-written packed depth textures are unsupported";
    return false;
  }
  RareTimer timer(timing.texture_ms);
  ++timing.new_textures;
  std::vector<DecodedSlice> slices;
  if (!DecodeTexture(texture.info, capture, slices, error)) return false;
  const bool compressed = IsCompressed(texture.format);
  const bool volume = texture.dimension == wgpu::TextureViewDimension::e3D;
  for (const auto& slice : slices) {
    if (slice.level >= texture.mip_levels) continue;
    const uint32_t mip_width = std::max(1u, texture.width >> slice.level);
    const uint32_t mip_height = std::max(1u, texture.height >> slice.level);
    uint32_t width = std::min(slice.width, mip_width);
    uint32_t height = std::min(slice.height, mip_height);
    if (compressed) {
      // Copies of block-compressed levels cover whole blocks of the level's
      // physical (block-aligned) size.
      width = std::min((slice.width + 3) & ~3u, (mip_width + 3) & ~3u);
      height = std::min((slice.height + 3) & ~3u, (mip_height + 3) & ~3u);
    }
    wgpu::TexelCopyTextureInfo destination{};
    destination.texture = texture.texture;
    destination.mipLevel = slice.level;
    destination.origin = {0, 0, volume ? 0 : slice.layer};
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = slice.row_pitch;
    layout.rowsPerImage = slice.rows;
    wgpu::Extent3D size{width, height, volume ? slice.depth : 1};
    queue.WriteTexture(&destination, slice.bytes.data(), slice.bytes.size(), &layout, &size);
    timing.texture_bytes += slice.bytes.size();
  }
  texture.content_serial = ++content_serial;
  return true;
}

std::shared_ptr<TextureResource> Renderer::State::SampledTexture(
    uint32_t handle, const xenos::xe_gpu_texture_fetch_t& fetch,
    const std::shared_ptr<const TextureCapture>& capture, std::string& error) {
  if (auto found = textures.find(handle); found != textures.end()) {
    if (found->second->gpu_produced) return found->second;
    if (capture && found->second->capture_generation == capture->generation) return found->second;
  }
  if (!capture) return {};
  auto resource = CreateTexture(capture->fetch, false, error);
  if (!resource || !Upload(*resource, *capture, error)) return {};
  resource->capture_generation = capture->generation;
  textures[handle] = resource;
  (void)fetch;
  return resource;
}

wgpu::Sampler Renderer::State::Sampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                                       const TextureResource* texture) {
  FetchKey fetch_key{};
  std::memcpy(fetch_key.data(), &fetch, sizeof(uint32_t) * 6);
  fetch_key[6] = texture ? texture->mip_levels : 0;
  if (auto found = fetch_samplers.find(fetch_key); found != fetch_samplers.end())
    return found->second;
  if (fetch_samplers.size() >= 65536) fetch_samplers.clear();
  auto& memo = fetch_samplers[fetch_key];
  xenos::ClampMode u, v, w;
  texture_util::GetClampModesForDimension(fetch, u, v, w);
  const auto filter = [](xenos::TextureFilter value) {
    return value == xenos::TextureFilter::kLinear ? wgpu::FilterMode::Linear
                                                  : wgpu::FilterMode::Nearest;
  };
  uint32_t minimum = 0, maximum = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr,
                                                 nullptr, &minimum, &maximum);
  if (fetch.mip_filter == xenos::TextureFilter::kBaseMap) maximum = minimum;
  if (texture) {
    minimum = std::min(minimum, texture->mip_levels - 1);
    maximum = std::clamp(maximum, minimum, texture->mip_levels - 1);
  }
  const auto min_filter = filter(fetch.min_filter);
  const auto mag_filter = filter(fetch.mag_filter);
  const auto mip_filter = fetch.mip_filter == xenos::TextureFilter::kLinear
                              ? wgpu::MipmapFilterMode::Linear
                              : wgpu::MipmapFilterMode::Nearest;
  uint16_t anisotropy = 1;
  if (fetch.aniso_filter != xenos::AnisoFilter::kDisabled &&
      fetch.aniso_filter != xenos::AnisoFilter::kUseFetchConst &&
      min_filter == wgpu::FilterMode::Linear && mag_filter == wgpu::FilterMode::Linear &&
      mip_filter == wgpu::MipmapFilterMode::Linear) {
    anisotropy = uint16_t(1u << std::min(4u, uint32_t(fetch.aniso_filter) - 1u));
  }
  const Words key{uint32_t(Address(u)),       uint32_t(Address(v)),  uint32_t(Address(w)),
                  uint32_t(min_filter),       uint32_t(mag_filter),  uint32_t(mip_filter),
                  minimum,                    maximum,               anisotropy};
  if (auto found = samplers.find(key); found != samplers.end()) return memo = found->second;
  wgpu::SamplerDescriptor descriptor{};
  descriptor.addressModeU = Address(u);
  descriptor.addressModeV = Address(v);
  descriptor.addressModeW = Address(w);
  descriptor.minFilter = min_filter;
  descriptor.magFilter = mag_filter;
  descriptor.mipmapFilter = mip_filter;
  descriptor.lodMinClamp = float(minimum);
  descriptor.lodMaxClamp = float(maximum);
  descriptor.maxAnisotropy = anisotropy;
  auto sampler = device.CreateSampler(&descriptor);
  samplers.emplace(key, sampler);
  return memo = sampler;
}

BufferPool::Range BufferPool::AllocateIn(uint32_t index, uint64_t size, uint64_t alignment) {
  auto& page = pages_[index];
  for (auto it = page.free.begin(); it != page.free.end(); ++it) {
    const uint64_t start = it->first, end = start + it->second;
    const uint64_t aligned = (start + alignment - 1) / alignment * alignment;
    if (aligned + size > end) continue;
    page.free.erase(it);
    if (aligned > start) page.free[start] = aligned - start;
    if (aligned + size < end) page.free[aligned + size] = end - aligned - size;
    return {index, aligned, size};
  }
  return {};
}

BufferPool::Range BufferPool::Allocate(uint64_t size, uint64_t alignment) {
  for (uint32_t i = 0; i < pages_.size(); ++i) {
    if (!pages_[i].buffer) continue;
    if (auto range = AllocateIn(i, size, alignment); range.page != UINT32_MAX) return range;
  }
  // Data larger than a page gets a page of its own, released once freed.
  uint32_t index = 0;
  while (index < pages_.size() && pages_[index].buffer) ++index;
  if (index == pages_.size()) pages_.emplace_back();
  auto& page = pages_[index];
  page.size = std::max(page_size_, (size + 3) & ~uint64_t(3));
  wgpu::BufferDescriptor descriptor{};
  descriptor.size = page.size;
  descriptor.usage = usage_;
  page.buffer = device_.CreateBuffer(&descriptor);
  page.free = {{0, page.size}};
  return AllocateIn(index, size, alignment);
}

void BufferPool::Free(const Range& range) {
  if (range.page >= pages_.size() || !pages_[range.page].buffer) return;
  auto& page = pages_[range.page];
  uint64_t start = range.offset, end = range.offset + range.size;
  auto next = page.free.lower_bound(start);
  if (next != page.free.end() && next->first == end) {
    end += next->second;
    next = page.free.erase(next);
  }
  if (next != page.free.begin()) {
    const auto previous = std::prev(next);
    if (previous->first + previous->second == start) {
      start = previous->first;
      page.free.erase(previous);
    }
  }
  page.free[start] = end - start;
  if (page.size > page_size_ && end - start == page.size) page = {};
}

wgpu::Buffer Renderer::State::IndexBuffer(const BufferCapture& capture, bool& index32,
                                          uint64_t& base, std::string& error) {
  if ((capture.flags & 0xFu) != 2) {
    error = "Index buffer has the wrong resource type";
    return nullptr;
  }
  index32 = (capture.flags & 0x80000000u) != 0;
  auto& pool = index32 ? index32_pool : index16_pool;
  if (auto found = index_buffers.find(capture.generation); found != index_buffers.end()) {
    found->second.frame = frame;
    base = found->second.range.offset;
    return pool.buffer(found->second.range.page);
  }
  RareTimer timer(timing.geometry_ms);
  ++timing.new_buffers;
  const size_t element = index32 ? 4 : 2;
  const size_t count = capture.bytes.size() / element;
  const uint64_t size = std::max<uint64_t>(4, (count * element + 3) & ~uint64_t(3));
  auto& converted = convert_scratch;
  converted.assign(size, 0);
  for (size_t i = 0; i < count; ++i) {
    if (index32) {
      uint32_t value;
      std::memcpy(&value, capture.bytes.data() + i * 4, 4);
      value = __builtin_bswap32(value) & 0x00FFFFFFu;
      std::memcpy(converted.data() + i * 4, &value, 4);
    } else {
      uint16_t value;
      std::memcpy(&value, capture.bytes.data() + i * 2, 2);
      value = __builtin_bswap16(value);
      std::memcpy(converted.data() + i * 2, &value, 2);
    }
  }
  const auto range = pool.Allocate(size, 4);
  if (range.page == UINT32_MAX) {
    error = "Index pool allocation failed";
    return nullptr;
  }
  queue.WriteBuffer(pool.buffer(range.page), range.offset, converted.data(), size);
  timing.buffer_bytes += size;
  index_buffers[capture.generation] = {range, frame, index32};
  base = range.offset;
  return pool.buffer(range.page);
}

bool Renderer::State::VertexBuffer(const BufferCapture& capture, uint32_t offset, uint32_t stride,
                                   std::span<const VertexElement* const> elements,
                                   VertexBinding& binding, std::string& error) {
  if ((capture.flags & 0xFu) != 1 || !stride || offset >= capture.bytes.size()) {
    error = "Vertex buffer range or type mismatch";
    return false;
  }
  Words& key = buffer_key;
  key = {uint32_t(capture.generation), uint32_t(capture.generation >> 32), offset, stride};
  for (const auto* element : elements) {
    key.push_back(element->offset);
    key.push_back(element->type);
  }
  if (auto found = vertex_buffers.find(key); found != vertex_buffers.end()) {
    found->second.frame = frame;
    const auto& range = found->second.range;
    binding = {vertex_pool.buffer(range.page), range.offset, range.size};
    return true;
  }
  const size_t vertices = (capture.bytes.size() - offset) / stride;
  if (!vertices) {
    error = "Vertex stream holds no complete vertex";
    return false;
  }
  RareTimer timer(timing.geometry_ms);
  ++timing.new_buffers;
  const size_t output_stride = elements.size() * 16;
  const uint64_t size = vertices * output_stride;
  auto& converted = convert_scratch;
  converted.resize(size);
  auto* destination = reinterpret_cast<float*>(converted.data());
  for (size_t vertex = 0; vertex < vertices; ++vertex) {
    const uint8_t* source = capture.bytes.data() + offset + vertex * stride;
    for (size_t i = 0; i < elements.size(); ++i) {
      float* out = destination + (vertex * elements.size() + i) * 4;
      const auto* element = elements[i];
      if (element->offset + VertexElementSize(element->type) > stride ||
          !DecodeVertexElement(element->type, source + element->offset, out)) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
      }
    }
  }
  const auto range = vertex_pool.Allocate(size, output_stride);
  if (range.page == UINT32_MAX) {
    error = "Vertex pool allocation failed";
    return false;
  }
  queue.WriteBuffer(vertex_pool.buffer(range.page), range.offset, converted.data(), size);
  timing.buffer_bytes += size;
  vertex_buffers[key] = {range, frame};
  binding = {vertex_pool.buffer(range.page), range.offset, range.size};
  return true;
}

std::shared_ptr<TextureResource> Renderer::State::PackedDepthAlias(
    uint32_t handle, uint32_t source_handle, const xenos::xe_gpu_texture_fetch_t& fetch,
    std::string& error) {
  const auto source = textures.find(source_handle);
  if (source == textures.end() || !source->second->gpu_produced || !source->second->depth_values ||
      !source->second->content_serial) {
    error = "Packed depth alias has no resolved depth snapshot";
    return {};
  }
  std::shared_ptr<TextureResource> alias;
  if (auto found = textures.find(handle); found != textures.end() && found->second->gpu_produced &&
      found->second->width == source->second->width &&
      found->second->height == source->second->height) {
    alias = found->second;
  } else {
    alias = CreateTexture(fetch, true, error);
    if (!alias) return {};
    if (alias->format != wgpu::TextureFormat::RGBA8Unorm ||
        alias->width != source->second->width || alias->height != source->second->height) {
      error = "Packed depth alias does not match its depth snapshot";
      return {};
    }
    textures[handle] = alias;
  }
  if (alias->packed_source_serial == source->second->content_serial &&
      alias->packed_swizzle == fetch.swizzle)
    return alias;
  wgpu::TextureViewDescriptor level{};
  level.dimension = wgpu::TextureViewDimension::e2D;
  level.mipLevelCount = 1;
  level.arrayLayerCount = 1;
  const bool float_depth =
      GetBaseFormat(source->second->info.format) == xenos::TextureFormat::k_24_8_FLOAT;
  const std::array<float, 12> parameters{0, 0, 0, 0, 0, 0, 0, 0,
                                         float_depth ? 1.0f : 0.0f, float(fetch.swizzle), 0, 0};
  if (!UtilityPass("packed_depth_alias", alias->texture.CreateView(&level),
                   wgpu::kDepthSliceUndefined, false, alias->format, alias->width, alias->height,
                   {0, 0, int32_t(alias->width), int32_t(alias->height)},
                   source->second->texture.CreateView(&level), parameters, false, error))
    return {};
  alias->packed_source_serial = source->second->content_serial;
  alias->packed_swizzle = fetch.swizzle;
  alias->content_serial = ++content_serial;
  return alias;
}

void Renderer::State::ReleaseResource(uint32_t handle) {
  textures.erase(handle);
  packed_depth_aliases.erase(handle);
  if (auto found = surfaces.find(handle); found != surfaces.end()) {
    GuestSurfaceView view{};
    if (DecodeGuestSurfaceView(found->second->descriptor, false, view)) {
      auto group = color_views.find(GetGuestPlacementKey(view));
      if (group != color_views.end()) std::erase(group->second, handle);
    }
    surfaces.erase(found);
  }
}

void Renderer::State::ClearResources() {
  surfaces.clear();
  color_views.clear();
  textures.clear();
  packed_depth_aliases.clear();
  vertex_buffers.clear();
  index_buffers.clear();
  vertex_pool.Clear();
  index16_pool.Clear();
  index32_pool.Clear();
  texture_groups.clear();
}

void Renderer::State::BeginFrame() {
  ++frame;
  WarmModules();
  SaveRecipes();
  std::erase_if(texture_groups,
                [&](const auto& entry) { return frame - entry.second.second > kGroupRetainFrames; });
  // Freed ranges are reused by later queue writes, which run after every
  // command buffer already submitted; nothing in the open batch uses them.
  std::erase_if(vertex_buffers, [&](const auto& entry) {
    if (frame - entry.second.frame <= kCacheRetainFrames) return false;
    vertex_pool.Free(entry.second.range);
    return true;
  });
  std::erase_if(index_buffers, [&](const auto& entry) {
    if (frame - entry.second.frame <= kCacheRetainFrames) return false;
    (entry.second.index32 ? index32_pool : index16_pool).Free(entry.second.range);
    return true;
  });
}

}  // namespace rex::graphics::gta4_webgpu
