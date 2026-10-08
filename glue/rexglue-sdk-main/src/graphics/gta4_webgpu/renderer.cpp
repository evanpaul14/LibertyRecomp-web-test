#include "renderer_state.h"
#include "simd_bytes.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(webgpu_perf_report, false, "GPU/Diagnostics",
                    "Web build: log renderer timing reports as warnings (shown with "
                    "--log_level=warn)");
REXCVAR_DEFINE_UINT32(webgpu_trace_frame, 0, "GPU/Diagnostics",
                      "Web build: log every title command of this presented frame");

namespace rex::graphics::gta4_webgpu {
namespace {
using namespace gta4_native;

constexpr uint64_t kUniformStride = 9728;  // Bytes used per draw, 256-aligned.
constexpr uint64_t kInitialUniformArena = 16u * 1024u * 1024u;
constexpr uint64_t kInitialGeometryArena = 8u * 1024u * 1024u;
constexpr uint64_t kMaximumArena = 256u * 1024u * 1024u;

std::string_view View(wgpu::StringView text) {
  return text.data ? std::string_view(text.data, text.length == WGPU_STRLEN ? std::strlen(text.data)
                                                                             : text.length)
                   : std::string_view{};
}

// Utility passes. Group 0 is the title uniform layout (params at its start).
constexpr char kUtilityCommon[] = R"(
struct Params { a: vec4<f32>, b: vec4<f32>, c: vec4<f32>, d: vec4<f32> }
@group(0) @binding(0) var<uniform> params: Params;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}
fn source_coord(p: vec4<f32>) -> vec2<i32> {
  // a: source origin, destination origin; b: source extent, destination extent.
  let rel = (p.xy - params.a.zw) * params.b.xy / params.b.zw;
  return vec2<i32>(params.a.xy + floor(rel));
}
// The guest's 24-bit depth encodings (unorm, or 20e4 float when c.x is set).
fn depth20e4(depth: f32) -> u32 {
  if (!(depth > 0.0)) { return 0u; }
  var bits = bitcast<u32>(depth);
  if (bits >= 0x3FFFFFF8u) { return 0xFFFFFFu; }
  if (bits < 0x38800000u) {
    let shift = min(113u - (bits >> 23u), 24u);
    bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
  } else {
    bits += 0xC8000000u;
  }
  return (bits >> 3u) & 0xFFFFFFu;
}
fn packed_depth(depth: f32) -> u32 {
  return select(u32(round(clamp(depth, 0.0, 1.0) * 16777215.0)), depth20e4(depth),
                params.c.x != 0.0);
}
)";
constexpr char kCopyColor[] = R"(
@group(1) @binding(0) var source: texture_2d<f32>;
@fragment fn fs(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> {
  return textureLoad(source, source_coord(p), 0) * params.c.x;
}
)";
// A color resolve through a surface whose MSAA layout differs from the one
// that holds the EDRAM contents, as gta4_native/resolve_convert_ps.glsl: the
// selected samples of each requested pixel are fetched from the owner (one
// value per guest pixel) and averaged. a: source origin (requested pixels),
// destination origin; b: requested and owner sample scales; c.x: exponent
// scale, c.y: CopySampleSelect.
constexpr char kResolveColor[] = R"(
@group(1) @binding(0) var source: texture_2d<f32>;
fn requested_sample(pixel: vec2<i32>, index: u32) -> vec4<f32> {
  let scale = vec2<i32>(params.b.xy);
  let offset = vec2<i32>(select(0, i32((index >> 1u) & 1u), scale.x == 2),
                         select(0, i32(index & 1u), scale.y == 2));
  let owner = (pixel * scale + offset) / vec2<i32>(params.b.zw);
  return textureLoad(source, clamp(owner, vec2<i32>(0), vec2<i32>(textureDimensions(source)) - 1), 0);
}
@fragment fn fs(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> {
  let pixel = vec2<i32>(params.a.xy) + vec2<i32>(floor(p.xy)) - vec2<i32>(params.a.zw);
  let select_samples = u32(params.c.y);
  var color: vec4<f32>;
  if (select_samples <= 3u) {
    color = requested_sample(pixel, select_samples);
  } else if (select_samples == 4u) {
    color = (requested_sample(pixel, 0u) + requested_sample(pixel, 1u)) * 0.5;
  } else if (select_samples == 5u) {
    color = (requested_sample(pixel, 2u) + requested_sample(pixel, 3u)) * 0.5;
  } else {
    color = (requested_sample(pixel, 0u) + requested_sample(pixel, 1u) +
             requested_sample(pixel, 2u) + requested_sample(pixel, 3u)) * 0.25;
  }
  return color * params.c.x;
}
)";
// Resolved depth keeps its stencil in green (see SampledFormat).
constexpr char kCopyDepthToColor[] = R"(
@group(1) @binding(0) var source: texture_depth_2d;
@group(1) @binding(1) var stencil: texture_2d<u32>;
@fragment fn fs(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> {
  let coord = source_coord(p);
  return vec4<f32>(textureLoad(source, coord, 0), f32(textureLoad(stencil, coord, 0).r), 0.0, 1.0);
}
)";
// A resolved depth/stencil snapshot read back as A8R8G8B8 (sub_828D9768), as
// gta4_native/packed_depth_alias_ps.glsl. c.x: 20e4 float depth, c.y: the
// fetch constant's swizzle.
constexpr char kPackedDepthAlias[] = R"(
@group(1) @binding(0) var source: texture_2d<f32>;
@fragment fn fs(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> {
  let value = textureLoad(source, vec2<i32>(p.xy), 0);
  let packed = (packed_depth(value.r) << 8u) | (u32(value.g) & 255u);
  let raw = vec4<f32>(vec4<u32>(packed, packed >> 8u, packed >> 16u, packed >> 24u) & vec4<u32>(255u)) / 255.0;
  let swizzle = u32(params.c.y);
  var color: vec4<f32>;
  for (var channel = 0u; channel < 4u; channel++) {
    let component = (swizzle >> (3u * channel)) & 7u;
    color[channel] = select(f32(component & 1u), raw[min(component, 3u)], component < 4u);
  }
  return color;
}
)";
constexpr char kCopyDepth[] = R"(
@group(1) @binding(0) var source: texture_depth_2d;
@fragment fn fs(@builtin(position) p: vec4<f32>) -> @builtin(frag_depth) f32 {
  return textureLoad(source, source_coord(p), 0);
}
)";
// Depth from a resolved snapshot (rg32float, depth in red).
constexpr char kCopyDepthValues[] = R"(
@group(1) @binding(0) var source: texture_2d<f32>;
@fragment fn fs(@builtin(position) p: vec4<f32>) -> @builtin(frag_depth) f32 {
  return textureLoad(source, vec2<i32>(p.xy), 0).r;
}
)";
// The scene-to-forward handoff, as gta4_native/scene_depth_handoff_ps.glsl:
// the pass clears stencil to kForwardEmptySceneStencil and its stencil state
// replaces it with kForwardCoveredSceneStencil wherever packed depth is nonzero.
constexpr char kSceneDepthHandoff[] = R"(
@group(1) @binding(0) var source: texture_2d<f32>;
@fragment fn fs(@builtin(position) p: vec4<f32>) -> @builtin(frag_depth) f32 {
  let depth = textureLoad(source, vec2<i32>(p.xy), 0).r;
  if (packed_depth(depth) == 0u) { discard; }
  return depth;
}
)";
constexpr char kClearColor[] = R"(
@fragment fn fs() -> @location(0) vec4<f32> { return params.c; }
)";
constexpr char kClearDepth[] = R"(
@fragment fn fs() -> @builtin(frag_depth) f32 { return params.c.x; }
)";
constexpr char kPresent[] = R"(
@group(1) @binding(0) var source: texture_2d<f32>;
@group(1) @binding(1) var source_sampler: sampler;
@fragment fn fs(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> {
  return vec4<f32>(textureSampleLevel(source, source_sampler, p.xy / params.b.zw, 0.0).rgb, 1.0);
}
)";

struct UtilityKind {
  const char* name;
  const char* code;
  // Group 1 source: 0 none, 1 float texture, 2 depth texture, 3 float texture + sampler,
  // 4 depth texture + stencil texture.
  int source;
  // Writes kForwardCoveredSceneStencil where it passes, over a stencil
  // cleared to kForwardEmptySceneStencil.
  bool scene_coverage = false;
};
constexpr UtilityKind kUtilities[] = {
    {"copy_color", kCopyColor, 1},     {"resolve_color", kResolveColor, 1},
    {"copy_depth_to_color", kCopyDepthToColor, 4},
    {"copy_depth", kCopyDepth, 2},     {"copy_depth_values", kCopyDepthValues, 1},
    {"clear_color", kClearColor, 0},   {"clear_depth", kClearDepth, 0},
    {"present", kPresent, 3},          {"packed_depth_alias", kPackedDepthAlias, 1},
    {"scene_depth_handoff", kSceneDepthHandoff, 1, true},
};
const UtilityKind* FindUtility(std::string_view name) {
  for (const auto& kind : kUtilities)
    if (name == kind.name) return &kind;
  return nullptr;
}
}  // namespace

Renderer::Renderer(memory::Memory* memory, const ShaderArchive* archive)
    : state_(std::make_unique<State>(memory, archive)) {}
Renderer::~Renderer() = default;

void Renderer::SetResume(std::function<void()> resume) { state_->resume = std::move(resume); }

uint32_t Renderer::max_texture_dimension() const {
  return state_->ready ? state_->limits.maxTextureDimension2D : 8192;
}

void Renderer::Initialize(std::function<void(bool, const std::string&)> done) {
  auto& s = *state_;
  s.instance = wgpu::CreateInstance(nullptr);
  if (!s.instance) {
    done(false, "WebGPU instance creation failed");
    return;
  }
  wgpu::RequestAdapterOptions options{};
  options.powerPreference = wgpu::PowerPreference::HighPerformance;
  s.instance.RequestAdapter(
      &options, wgpu::CallbackMode::AllowSpontaneous,
      [this, done](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter,
                   wgpu::StringView message) {
        auto& s = *state_;
        if (status != wgpu::RequestAdapterStatus::Success || !adapter) {
          done(false, fmt::format("WebGPU adapter unavailable: {}", View(message)));
          return;
        }
        s.adapter = adapter;
        wgpu::Limits supported{};
        adapter.GetLimits(&supported);
        std::vector<wgpu::FeatureName> features;
        for (auto feature : {wgpu::FeatureName::Float32Filterable, wgpu::FeatureName::ClipDistances,
                             wgpu::FeatureName::Depth32FloatStencil8,
                             wgpu::FeatureName::TextureCompressionBC,
                             wgpu::FeatureName::Float32Blendable}) {
          if (adapter.HasFeature(feature)) features.push_back(feature);
        }
        const auto has = [&](wgpu::FeatureName feature) {
          return std::find(features.begin(), features.end(), feature) != features.end();
        };
        if (!has(wgpu::FeatureName::ClipDistances) || !has(wgpu::FeatureName::Float32Filterable)) {
          done(false, "WebGPU adapter lacks clip-distances or float32-filterable");
          return;
        }
        s.depth_format = has(wgpu::FeatureName::Depth32FloatStencil8)
                             ? wgpu::TextureFormat::Depth32FloatStencil8
                             : wgpu::TextureFormat::Depth24PlusStencil8;
        s.float32_blendable = has(wgpu::FeatureName::Float32Blendable);
        s.bc_textures = has(wgpu::FeatureName::TextureCompressionBC);
        // Title vertex shaders pass up to 18 varyings.
        wgpu::Limits required{};
        required.maxInterStageShaderVariables = supported.maxInterStageShaderVariables;
        required.maxTextureDimension2D = supported.maxTextureDimension2D;
        required.maxColorAttachmentBytesPerSample = supported.maxColorAttachmentBytesPerSample;
        required.maxBufferSize = supported.maxBufferSize;
        wgpu::DeviceDescriptor descriptor{};
        descriptor.requiredFeatureCount = features.size();
        descriptor.requiredFeatures = features.data();
        descriptor.requiredLimits = &required;
        descriptor.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
              static std::atomic<uint64_t> errors{0};
              const uint64_t count = ++errors;
              if (count <= 64 || count % 1024 == 0)
                REXLOG_ERROR("gta4-webgpu: GPU error #{} type={}: {}", count, uint32_t(type),
                             View(message));
            });
        descriptor.SetDeviceLostCallback(
            wgpu::CallbackMode::AllowSpontaneous,
            [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
              REXLOG_ERROR("gta4-webgpu: device lost ({}): {}", uint32_t(reason), View(message));
            });
        adapter.RequestDevice(
            &descriptor, wgpu::CallbackMode::AllowSpontaneous,
            [this, done](wgpu::RequestDeviceStatus status, wgpu::Device device,
                         wgpu::StringView message) {
              auto& s = *state_;
              if (status != wgpu::RequestDeviceStatus::Success || !device) {
                done(false, fmt::format("WebGPU device creation failed: {}", View(message)));
                return;
              }
              s.device = device;
              s.queue = device.GetQueue();
              device.GetLimits(&s.limits);
              s.InitializeDevice();
              s.ready = true;
              wgpu::AdapterInfo info{};
              s.adapter.GetInfo(&info);
              REXLOG_INFO("gta4-webgpu: device ready; adapter='{}' '{}' depth={} bc={}",
                          View(info.vendor), View(info.device),
                          s.depth_format == wgpu::TextureFormat::Depth32FloatStencil8
                              ? "depth32float-stencil8" : "depth24plus-stencil8",
                          s.bc_textures);
              done(true, {});
            });
      });
}

void Renderer::State::InitializeDevice() {
  wgpu::BindGroupLayoutEntry uniform{};
  uniform.binding = 0;
  uniform.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  uniform.buffer.type = wgpu::BufferBindingType::Uniform;
  uniform.buffer.hasDynamicOffset = true;
  uniform.buffer.minBindingSize = kUniformBlockSize;
  wgpu::BindGroupLayoutDescriptor layout{};
  layout.entryCount = 1;
  layout.entries = &uniform;
  uniform_layout = device.CreateBindGroupLayout(&layout);

  wgpu::BufferDescriptor zero{};
  zero.size = 64;
  zero.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  zero.mappedAtCreation = true;
  zero_vertices = device.CreateBuffer(&zero);
  std::memset(zero_vertices.GetMappedRange(), 0, 64);
  zero_vertices.Unmap();

  const auto fallback = [&](uint32_t layers, wgpu::TextureViewDimension dimension,
                            wgpu::Texture& texture, wgpu::TextureView& view) {
    wgpu::TextureDescriptor descriptor{};
    descriptor.size = {1, 1, layers};
    descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
    descriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    texture = device.CreateTexture(&descriptor);
    const uint32_t black = 0;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      wgpu::TexelCopyTextureInfo destination{};
      destination.texture = texture;
      destination.origin = {0, 0, layer};
      wgpu::TexelCopyBufferLayout data{};
      data.bytesPerRow = 4;
      wgpu::Extent3D size{1, 1, 1};
      queue.WriteTexture(&destination, &black, sizeof(black), &data, &size);
    }
    wgpu::TextureViewDescriptor view_descriptor{};
    view_descriptor.dimension = dimension;
    view = texture.CreateView(&view_descriptor);
  };
  fallback(1, wgpu::TextureViewDimension::e2D, fallback_2d, fallback_2d_view);
  fallback(6, wgpu::TextureViewDimension::Cube, fallback_cube, fallback_cube_view);

  wgpu::SamplerDescriptor sampler{};
  fallback_sampler = device.CreateSampler(&sampler);
  sampler.minFilter = sampler.magFilter = wgpu::FilterMode::Linear;
  linear_sampler = device.CreateSampler(&sampler);
}

bool Renderer::State::Begin(std::string& error) {
  if (!ready) {
    error = "WebGPU device is not ready";
    return false;
  }
  if (!encoder) encoder = device.CreateCommandEncoder();
  return true;
}

void Renderer::State::EndPass() {
  if (pass) {
    pass.End();
    pass = nullptr;
  }
  pass_targets = {};
  pass_state = {};
}

uint64_t Renderer::State::PushUniforms(std::span<const uint8_t> bytes, std::string& error) {
  if (bytes.size() > kUniformStride) {
    error = "Uniform block exceeds its slot";
    return UINT64_MAX;
  }
  uint64_t offset;
  uint8_t* slot = ReserveUniforms(offset, error);
  if (!slot) return UINT64_MAX;
  std::memcpy(slot, bytes.data(), bytes.size());
  return CommitUniforms(offset, bytes.size());
}

uint8_t* Renderer::State::ReserveUniforms(uint64_t& offset, std::string& error) {
  // One kUniformStride slot per draw. Bindings span kUniformBlockSize from the
  // slot start, so the buffer keeps that much headroom past the last slot.
  // Shaders read only what the caller writes, so stale bytes past it from an
  // earlier batch are harmless.
  if (uniforms.used + kUniformStride + kUniformBlockSize > uniforms.capacity) {
    if (uniforms.used && !Flush(error)) return nullptr;
    if (!Begin(error)) return nullptr;
    if (kUniformStride + kUniformBlockSize > uniforms.capacity) {
      uniforms.capacity = kInitialUniformArena;
      uniforms.bytes.assign(uniforms.capacity, 0);
      wgpu::BufferDescriptor descriptor{};
      descriptor.size = uniforms.capacity;
      descriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
      uniforms.buffer = device.CreateBuffer(&descriptor);
      wgpu::BindGroupEntry entry{};
      entry.binding = 0;
      entry.buffer = uniforms.buffer;
      entry.size = kUniformBlockSize;
      wgpu::BindGroupDescriptor group{};
      group.layout = uniform_layout;
      group.entryCount = 1;
      group.entries = &entry;
      uniform_group = device.CreateBindGroup(&group);
    }
  }
  offset = uniforms.used;
  return uniforms.bytes.data() + offset;
}

uint64_t Renderer::State::CommitUniforms(uint64_t offset, size_t size) {
  // Identical constants share a slot within the batch: the reserved slot is
  // dropped again.
  const uint8_t* bytes = uniforms.bytes.data() + offset;
  const uint64_t hash = XXH3_64bits(bytes, size);
  if (auto found = uniform_slots.find(hash); found != uniform_slots.end() &&
      found->second.size == size &&
      BytesEqual(uniforms.bytes.data() + found->second.offset, bytes, size)) {
    ++timing.uniform_reused;
    return found->second.offset;
  }
  uniforms.used = offset + kUniformStride;
  uniform_slots[hash] = {offset, size};
  ++timing.uniform_slots;
  return offset;
}

void Renderer::State::ApplyDeviceDelta(const DeviceDelta& delta) {
  auto& block = devices[delta.device];
  if (block.empty()) block.resize(kGuestDeviceSize);
  // Chunks holding vertex or pixel constants.
  constexpr uint32_t kFirstConstantChunk = 0x780 / kDeviceChunkBytes;
  constexpr uint32_t kLastConstantChunk = (0x1780 + 0xE00 - 1) / kDeviceChunkBytes;
  const uint8_t* source = delta.bytes.data();
  for (uint32_t chunk = 0; chunk < kDeviceChunkCount; ++chunk) {
    if (!(delta.chunks[chunk / 32] & (1u << (chunk % 32)))) continue;
    std::memcpy(block.data() + size_t(chunk) * kDeviceChunkBytes, source, kDeviceChunkBytes);
    source += kDeviceChunkBytes;
    if (chunk >= kFirstConstantChunk && chunk <= kLastConstantChunk) ++constants_serial;
  }
}

uint64_t Renderer::State::PushGeometry(std::span<const uint8_t> bytes, std::string& error) {
  const uint64_t size = (bytes.size() + 15) & ~uint64_t(15);
  if (size > kMaximumArena) {
    error = "Per-draw geometry exceeds its bound";
    return UINT64_MAX;
  }
  if (geometry.used + size > geometry.capacity) {
    if (geometry.used && !Flush(error)) return UINT64_MAX;
    if (!Begin(error)) return UINT64_MAX;
    if (size > geometry.capacity) {
      geometry.capacity = std::max<uint64_t>(kInitialGeometryArena, size * 2);
      geometry.bytes.assign(geometry.capacity, 0);
      wgpu::BufferDescriptor descriptor{};
      descriptor.size = geometry.capacity;
      descriptor.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::Index |
                         wgpu::BufferUsage::CopyDst;
      geometry.buffer = device.CreateBuffer(&descriptor);
    }
  }
  const uint64_t offset = geometry.used;
  geometry.used += size;
  std::memcpy(geometry.bytes.data() + offset, bytes.data(), bytes.size());
  return offset;
}

bool Renderer::State::Flush(std::string& error) {
  EndPass();
  uniform_slots.clear();
  last_uniforms.offset = UINT64_MAX;
  if (!encoder) return true;
  ScopedTimer timer(timing.submit_ms);
  // Arena writes are ordered before the command buffer that reads them.
  if (uniforms.used) queue.WriteBuffer(uniforms.buffer, 0, uniforms.bytes.data(), uniforms.used);
  if (geometry.used) queue.WriteBuffer(geometry.buffer, 0, geometry.bytes.data(), geometry.used);
  uniforms.used = 0;
  geometry.used = 0;
  auto commands = encoder.Finish();
  encoder = nullptr;
  queue.Submit(1, &commands);
  (void)error;
  return true;
}

bool Renderer::State::BeginPass(const Targets& targets, std::string& error) {
  if (pass && pass_targets == targets) return true;
  EndPass();
  if (!Begin(error)) return false;
  std::array<wgpu::RenderPassColorAttachment, kRenderTargetCount> colors{};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    const auto& surface = targets.colors[i];
    if (!surface) continue;
    color_count = i + 1;
    colors[i].view = surface->view;
    colors[i].loadOp = surface->initialized ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    colors[i].storeOp = wgpu::StoreOp::Store;
    colors[i].clearValue = {0, 0, 0, 0};
    surface->initialized = true;
  }
  wgpu::RenderPassDepthStencilAttachment depth{};
  wgpu::RenderPassDescriptor descriptor{};
  descriptor.colorAttachmentCount = color_count;
  descriptor.colorAttachments = colors.data();
  if (targets.depth) {
    depth.view = targets.depth->view;
    depth.depthLoadOp = depth.stencilLoadOp =
        targets.depth->initialized ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    depth.depthStoreOp = depth.stencilStoreOp = wgpu::StoreOp::Store;
    depth.depthClearValue = 0.0f;
    depth.stencilClearValue = 0;
    targets.depth->initialized = true;
    descriptor.depthStencilAttachment = &depth;
  }
  pass = encoder.BeginRenderPass(&descriptor);
  pass_targets = targets;
  pass_state = {};
  return true;
}

wgpu::ShaderModule Renderer::State::Module(const ShaderRecord& record, bool late,
                                           std::string& error) {
  const uint64_t key = record.hash * 4 + uint64_t(record.stage) * 2 + (late ? 1 : 0);
  if (auto found = modules.find(key); found != modules.end()) return found->second;
  const std::string_view code = late ? record.late : record.early;
  if (code.empty()) {
    error = "Shader variant is missing from the archive";
    return nullptr;
  }
  wgpu::ShaderSourceWGSL source{};
  source.code = wgpu::StringView(code.data(), code.size());
  wgpu::ShaderModuleDescriptor descriptor{};
  descriptor.nextInChain = &source;
  const std::string label = fmt::format("title {:016X}{}", record.hash, late ? " late" : "");
  descriptor.label = wgpu::StringView(label.data(), label.size());
  auto module = device.CreateShaderModule(&descriptor);
  modules.emplace(key, module);
  return module;
}

wgpu::ShaderModule Renderer::State::UtilityModule(const char* name, const char* code) {
  if (auto found = utility_modules.find(name); found != utility_modules.end()) return found->second;
  const std::string text = std::string(kUtilityCommon) + code;
  wgpu::ShaderSourceWGSL source{};
  source.code = wgpu::StringView(text.data(), text.size());
  wgpu::ShaderModuleDescriptor descriptor{};
  descriptor.nextInChain = &source;
  auto module = device.CreateShaderModule(&descriptor);
  utility_modules.emplace(name, module);
  return module;
}

wgpu::BindGroupLayout Renderer::State::TextureLayout(uint32_t texture_mask, uint32_t cube_mask,
                                                     uint32_t sampler_mask) {
  const std::array<uint32_t, 3> words{texture_mask, cube_mask & texture_mask, sampler_mask};
  const uint64_t full_key = XXH3_64bits(words.data(), sizeof(words));
  if (auto found = texture_layouts.find(full_key); found != texture_layouts.end())
    return found->second;
  std::vector<wgpu::BindGroupLayoutEntry> entries;
  for (uint32_t slot = 0; slot < kTextureStageCount; ++slot) {
    if (texture_mask & (1u << slot)) {
      wgpu::BindGroupLayoutEntry entry{};
      entry.binding = slot;
      entry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
      entry.texture.sampleType = wgpu::TextureSampleType::Float;
      entry.texture.viewDimension = (cube_mask & (1u << slot)) ? wgpu::TextureViewDimension::Cube
                                                               : wgpu::TextureViewDimension::e2D;
      entries.push_back(entry);
    }
    if (sampler_mask & (1u << slot)) {
      wgpu::BindGroupLayoutEntry entry{};
      entry.binding = kSamplerBindingBase + slot;
      entry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
      entry.sampler.type = wgpu::SamplerBindingType::Filtering;
      entries.push_back(entry);
    }
  }
  wgpu::BindGroupLayoutDescriptor descriptor{};
  descriptor.entryCount = entries.size();
  descriptor.entries = entries.data();
  auto layout = device.CreateBindGroupLayout(&descriptor);
  texture_layouts.emplace(full_key, layout);
  return layout;
}

wgpu::RenderPipeline Renderer::State::UtilityPipeline(const std::string& name,
                                                      wgpu::TextureFormat color,
                                                      wgpu::TextureFormat depth,
                                                      std::string& error) {
  const std::string key = fmt::format("{}:{}:{}", name, uint32_t(color), uint32_t(depth));
  if (auto found = utility_pipelines.find(key); found != utility_pipelines.end())
    return found->second;
  const auto* kind = FindUtility(name);
  if (!kind) {
    error = "Unknown utility pass " + name;
    return nullptr;
  }
  auto module = UtilityModule(kind->name, kind->code);
  std::vector<wgpu::BindGroupLayout> groups{uniform_layout};
  if (kind->source) {
    std::array<wgpu::BindGroupLayoutEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[0].texture.sampleType =
        kind->source == 2 || kind->source == 4 ? wgpu::TextureSampleType::Depth
        : kind->source == 3                    ? wgpu::TextureSampleType::Float
                                               : wgpu::TextureSampleType::UnfilterableFloat;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    if (kind->source == 4) {
      entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
      entries[1].texture.sampleType = wgpu::TextureSampleType::Uint;
    } else {
      entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    }
    wgpu::BindGroupLayoutDescriptor descriptor{};
    descriptor.entryCount = kind->source >= 3 ? 2 : 1;
    descriptor.entries = entries.data();
    groups.push_back(device.CreateBindGroupLayout(&descriptor));
  }
  wgpu::PipelineLayoutDescriptor layout{};
  layout.bindGroupLayoutCount = groups.size();
  layout.bindGroupLayouts = groups.data();
  wgpu::RenderPipelineDescriptor descriptor{};
  descriptor.layout = device.CreatePipelineLayout(&layout);
  descriptor.vertex.module = module;
  descriptor.vertex.entryPoint = "vs";
  wgpu::ColorTargetState target{};
  target.format = color;
  wgpu::FragmentState fragment{};
  fragment.module = module;
  fragment.entryPoint = "fs";
  fragment.targetCount = color == wgpu::TextureFormat::Undefined ? 0 : 1;
  fragment.targets = &target;
  descriptor.fragment = &fragment;
  wgpu::DepthStencilState depth_state{};
  if (depth != wgpu::TextureFormat::Undefined) {
    depth_state.format = depth;
    depth_state.depthWriteEnabled = wgpu::OptionalBool::True;
    depth_state.depthCompare = wgpu::CompareFunction::Always;
    depth_state.stencilFront = depth_state.stencilBack = {
        wgpu::CompareFunction::Always, wgpu::StencilOperation::Keep, wgpu::StencilOperation::Keep,
        kind->scene_coverage ? wgpu::StencilOperation::Replace : wgpu::StencilOperation::Keep};
    depth_state.stencilReadMask = depth_state.stencilWriteMask = kind->scene_coverage ? 0xFF : 0;
    descriptor.depthStencil = &depth_state;
  }
  auto pipeline = device.CreateRenderPipeline(&descriptor);
  utility_pipelines.emplace(key, pipeline);
  return pipeline;
}

bool Renderer::State::UtilityPass(const std::string& name, wgpu::TextureView target,
                                  uint32_t depth_slice, bool target_is_depth,
                                  wgpu::TextureFormat format, uint32_t width, uint32_t height,
                                  const std::array<int32_t, 4>& scissor, wgpu::TextureView source,
                                  std::span<const float> parameters, bool load_existing,
                                  std::string& error, wgpu::TextureView stencil) {
  auto pipeline = UtilityPipeline(name, target_is_depth ? wgpu::TextureFormat::Undefined : format,
                                  target_is_depth ? format : wgpu::TextureFormat::Undefined, error);
  if (!pipeline) return false;
  const bool scene_coverage = FindUtility(name)->scene_coverage;
  std::array<uint8_t, 64> params{};
  std::memcpy(params.data(), parameters.data(), std::min(params.size(), parameters.size_bytes()));
  const uint64_t offset = PushUniforms(params, error);
  if (offset == UINT64_MAX) return false;
  EndPass();
  if (!Begin(error)) return false;
  wgpu::RenderPassColorAttachment color{};
  wgpu::RenderPassDepthStencilAttachment depth{};
  wgpu::RenderPassDescriptor descriptor{};
  if (target_is_depth) {
    depth.view = target;
    depth.depthLoadOp = load_existing ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    depth.depthStoreOp = wgpu::StoreOp::Store;
    depth.depthClearValue = 0.0f;
    const bool stencil = format != wgpu::TextureFormat::Depth32Float;
    if (stencil) {
      depth.stencilLoadOp = load_existing ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
      depth.stencilStoreOp = wgpu::StoreOp::Store;
      if (scene_coverage) {
        depth.stencilLoadOp = wgpu::LoadOp::Clear;
        depth.stencilClearValue = kForwardEmptySceneStencil;
      }
    }
    descriptor.depthStencilAttachment = &depth;
  } else {
    color.view = target;
    color.depthSlice = depth_slice;
    color.loadOp = load_existing ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    descriptor.colorAttachmentCount = 1;
    descriptor.colorAttachments = &color;
  }
  auto encoder_pass = encoder.BeginRenderPass(&descriptor);
  encoder_pass.SetPipeline(pipeline);
  const uint32_t dynamic_offset = uint32_t(offset);
  encoder_pass.SetBindGroup(0, uniform_group, 1, &dynamic_offset);
  if (source) {
    std::array<wgpu::BindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].textureView = source;
    entries[1].binding = 1;
    if (stencil)
      entries[1].textureView = stencil;
    else
      entries[1].sampler = linear_sampler;
    wgpu::BindGroupDescriptor group{};
    group.layout = pipeline.GetBindGroupLayout(1);
    group.entryCount = name == "present" || stencil ? 2 : 1;
    group.entries = entries.data();
    encoder_pass.SetBindGroup(1, device.CreateBindGroup(&group));
  }
  if (scene_coverage) encoder_pass.SetStencilReference(kForwardCoveredSceneStencil);
  encoder_pass.SetViewport(0, 0, float(width), float(height), 0, 1);
  const int32_t left = std::clamp(scissor[0], 0, int32_t(width));
  const int32_t top = std::clamp(scissor[1], 0, int32_t(height));
  const int32_t right = std::clamp(scissor[2], left, int32_t(width));
  const int32_t bottom = std::clamp(scissor[3], top, int32_t(height));
  encoder_pass.SetScissorRect(left, top, right - left, bottom - top);
  if (right > left && bottom > top) encoder_pass.Draw(3);
  encoder_pass.End();
  return true;
}

Renderer::Status Renderer::Execute(Work& work, std::string& error) {
  auto& s = *state_;
  const auto finish_sync = [&](bool ok) {
    if (work.execute) {
      work.execute->error = error;
      work.execute->Finish(ok);
    }
  };
  // Every delta is applied, so the copies stay in step with the game's.
  if (work.device.device) s.ApplyDeviceDelta(work.device);
  if (!s.ready) {
    error = "WebGPU device is not ready";
    finish_sync(false);
    return Status::kDone;
  }
  const auto type = work.type();
  // Present reports and resets the stats itself, so it is timed there.
  std::optional<ScopedTimer> timer;
  if (type != CommandType::kPresent) timer.emplace(s.timing.execute_ms);
  if (uint32_t(type) < s.stats.commands.size()) ++s.stats.commands[uint32_t(type)];
  s.trace = REXCVAR_GET(webgpu_trace_frame) &&
            s.submitted_frame + 1 == REXCVAR_GET(webgpu_trace_frame);
  if (s.trace && type != CommandType::kDrawPrimitive && type != CommandType::kDrawIndexedPrimitive &&
      type != CommandType::kDrawPrimitiveUp)
    REXLOG_INFO("webgpu-trace: command type={}", uint32_t(type));
  switch (type) {
    case CommandType::kDeviceCreated: {
      const auto c = work.As<DeviceCommand>();
      if (c.mode != 2) {
        s.vertex_shader = s.pixel_shader = s.declaration = 0;
        s.streams = {};
      }
      return Status::kDone;
    }
    case CommandType::kDeviceDestroyed:
      s.Flush(error);
      s.ClearResources();
      s.shaders.clear();
      s.declarations.clear();
      s.input_layouts.clear();
      s.devices.clear();
      return Status::kDone;
    case CommandType::kRegisterShader: {
      const auto c = work.As<RegisterShaderCommand>();
      const auto* record = s.archive->Find(c.hash, c.stage);
      if (!record) {
        s.shaders.erase(c.shader);
        error = fmt::format("shader {:016X} is not in the WGSL archive", c.hash);
        return Status::kDone;
      }
      s.shaders[c.shader] = record;
      return Status::kDone;
    }
    case CommandType::kRegisterVertexDeclaration: {
      const auto c = work.As<RegisterVertexDeclarationCommand>();
      s.declarations[c.declaration].assign(c.elements, c.elements + c.element_count);
      s.input_layouts.clear();  // They point into the declarations.
      return Status::kDone;
    }
    case CommandType::kSetPixelShader:
      s.pixel_shader = work.As<SetShaderCommand>().shader;
      return Status::kDone;
    case CommandType::kSetVertexShader:
      s.vertex_shader = work.As<SetShaderCommand>().shader;
      return Status::kDone;
    case CommandType::kSetVertexDeclaration:
      s.declaration = work.As<SetVertexDeclarationCommand>().declaration;
      return Status::kDone;
    case CommandType::kSetVertexStream: {
      const auto c = work.As<SetVertexStreamCommand>();
      if (c.stream < kVertexStreamCount) s.streams[c.stream] = {c.offset, c.stride};
      return Status::kDone;
    }
    case CommandType::kReleaseResource: {
      const uint32_t handle = work.As<ReleaseResourceCommand>().resource;
      s.ReleaseResource(handle);
      s.shaders.erase(handle);
      if (s.declarations.erase(handle)) s.input_layouts.clear();
      return Status::kDone;
    }
    case CommandType::kUpdateEnvironmentalData:
      s.environment = work.As<UpdateEnvironmentalDataCommand>().data;
      return Status::kDone;
    case CommandType::kDrawPrimitive:
    case CommandType::kDrawPrimitiveUp:
    case CommandType::kDrawIndexedPrimitive: {
      ScopedTimer draw_timer(s.timing.draw_ms);
      if (!s.Draw(work, error)) {
        ++s.skipped_draws;
        ++s.stats.failed;
      }
      return Status::kDone;
    }
    case CommandType::kClear:
      s.Clear(work, error);
      return Status::kDone;
    case CommandType::kResolve:
      s.Resolve(work, error);
      return Status::kDone;
    case CommandType::kDepthSurfaceHandoff:
      s.Handoff(work, error);
      return Status::kDone;
    case CommandType::kPresent:
      return s.Present(work, error);
    case CommandType::kRegisterVirtualResource: {
      const auto c = work.As<RegisterVirtualResourceCommand>();
      if (c.kind == VirtualResourceKind::kTexture && c.packed_depth_source)
        s.packed_depth_aliases[c.resource] = c.packed_depth_source;
      else
        s.packed_depth_aliases.erase(c.resource);
      return Status::kDone;
    }
    case CommandType::kRenderPhaseMarker:
      if (s.trace) {
        const auto c = work.As<RenderPhaseMarkerCommand>();
        REXLOG_INFO("webgpu-trace: phase={} event={}", uint32_t(c.phase), uint32_t(c.event));
      }
      return Status::kDone;
    case CommandType::kTextureLock: {
      const auto status = s.Readback(work, error);
      if (status != Status::kPending) finish_sync(error.empty());
      return status;
    }
    default:
      // Render state, texture bindings and markers are read from the
      // captured device block when a draw executes.
      return Status::kDone;
  }
}

}  // namespace rex::graphics::gta4_webgpu
