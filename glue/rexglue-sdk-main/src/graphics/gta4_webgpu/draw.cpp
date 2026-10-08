#include "renderer_state.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../gta4_native/alpha_to_coverage_util.h"
#include "../gta4_native/core/geometry.h"
#include "../gta4_native/native_color_output.h"
#include "../gta4_native/native_fixed_function_policy.h"
#include "../gta4_native/native_shader_booleans.h"
#include "../gta4_native/native_triangle_fan.h"
#include "simd_bytes.h"
#include "vertex_decode.h"

REXCVAR_DEFINE_STRING(webgpu_trace_pixel, "", "GPU/Diagnostics",
                      "Web build: with --webgpu_trace_frame, log which draws change the "
                      "texel x,y of their first color target");
REXCVAR_DEFINE_STRING(webgpu_skip_pixel_shader, "", "GPU/Diagnostics",
                      "Web build: skip draws that use these pixel shader hashes (hex, comma-separated)");
REXCVAR_DEFINE_BOOL(webgpu_async_pipelines, true, "GPU",
                    "Web build: create title pipelines asynchronously and skip their draws "
                    "until they are ready (traced frames still create them synchronously)");

namespace rex::graphics::gta4_webgpu {
namespace {
using namespace gta4_native;
using FixedState = core::FixedFunctionState;

constexpr uint32_t kDeviceTextures = 0x30F8;
constexpr uint32_t kDeviceFetchConstants = 0x480;
constexpr uint32_t kDeviceVertexConstants = 0x780;
constexpr uint32_t kDevicePixelConstants = 0x1780;
constexpr uint32_t kPixelConstantBytes = 0xE00;
static_assert(kPixelConstantBytes <= kUniformPixelBytes);
static_assert(sizeof(core::SharedConstants) <= kUniformSpecializationOffset);
static_assert(kUniformSpecializationOffset + 4 <= kUniformSharedBytes);
static_assert(kUniformSharedBytes <= 1536);

wgpu::CompareFunction Compare(uint32_t value) {
  static constexpr wgpu::CompareFunction kValues[] = {
      wgpu::CompareFunction::Never,        wgpu::CompareFunction::Less,
      wgpu::CompareFunction::Equal,        wgpu::CompareFunction::LessEqual,
      wgpu::CompareFunction::Greater,      wgpu::CompareFunction::NotEqual,
      wgpu::CompareFunction::GreaterEqual, wgpu::CompareFunction::Always};
  return kValues[value & 7];
}
wgpu::StencilOperation StencilOp(uint32_t value) {
  static constexpr wgpu::StencilOperation kValues[] = {
      wgpu::StencilOperation::Keep,           wgpu::StencilOperation::Zero,
      wgpu::StencilOperation::Replace,        wgpu::StencilOperation::IncrementClamp,
      wgpu::StencilOperation::DecrementClamp, wgpu::StencilOperation::Invert,
      wgpu::StencilOperation::IncrementWrap,  wgpu::StencilOperation::DecrementWrap};
  return kValues[value & 7];
}
bool BlendFactor(uint32_t value, wgpu::BlendFactor& out) {
  switch (value) {
    case 0: out = wgpu::BlendFactor::Zero; return true;
    case 1: out = wgpu::BlendFactor::One; return true;
    case 4: out = wgpu::BlendFactor::Src; return true;
    case 5: out = wgpu::BlendFactor::OneMinusSrc; return true;
    case 6: out = wgpu::BlendFactor::SrcAlpha; return true;
    case 7: out = wgpu::BlendFactor::OneMinusSrcAlpha; return true;
    case 8: out = wgpu::BlendFactor::Dst; return true;
    case 9: out = wgpu::BlendFactor::OneMinusDst; return true;
    case 10: out = wgpu::BlendFactor::DstAlpha; return true;
    case 11: out = wgpu::BlendFactor::OneMinusDstAlpha; return true;
    // WebGPU has one blend constant for color and alpha.
    case 12: case 14: out = wgpu::BlendFactor::Constant; return true;
    case 13: case 15: out = wgpu::BlendFactor::OneMinusConstant; return true;
    case 16: out = wgpu::BlendFactor::SrcAlphaSaturated; return true;
    default: return false;
  }
}
bool BlendOperation(uint32_t value, wgpu::BlendOperation& out) {
  switch (value) {
    case 0: out = wgpu::BlendOperation::Add; return true;
    case 1: out = wgpu::BlendOperation::Subtract; return true;
    case 2: out = wgpu::BlendOperation::Min; return true;
    case 3: out = wgpu::BlendOperation::Max; return true;
    case 4: out = wgpu::BlendOperation::ReverseSubtract; return true;
    default: return false;
  }
}
bool Blendable(wgpu::TextureFormat format, bool float32_blendable) {
  return format != wgpu::TextureFormat::R32Float || float32_blendable;
}

template <class T>
std::span<const uint8_t> Bytes(const T& value) {
  return {reinterpret_cast<const uint8_t*>(&value), sizeof(value)};
}
}  // namespace

Pipeline* Renderer::State::DrawPipeline(
    const Targets& targets, const FixedState& fixed, const ShaderRecord& vertex,
    const ShaderRecord* pixel, bool late, uint32_t specialization, uint32_t topology,
    uint32_t strip_format, const InputLayout& inputs, uint32_t requested_colors, uint32_t used,
    std::string& error) {
  (void)specialization;
  const uint32_t texture_mask = vertex.texture_mask | (pixel ? pixel->texture_mask : 0);
  const uint32_t cube_mask = vertex.cube_mask | (pixel ? pixel->cube_mask : 0);
  const uint32_t sampler_mask = vertex.sampler_mask | (pixel ? pixel->sampler_mask : 0);
  Words& key = pipeline_key;
  key = {uint32_t(vertex.hash), uint32_t(vertex.hash >> 32),
            pixel ? uint32_t(pixel->hash) : 0u, pixel ? uint32_t(pixel->hash >> 32) : 0u,
            late, topology, strip_format, texture_mask, cube_mask, sampler_mask};
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    const auto& color = targets.colors[i];
    key.push_back(color ? uint32_t(color->format) : 0u);
    key.push_back(color && (used & (1u << i)) ? fixed.blend_controls[i] : 0u);
  }
  key.push_back(fixed.color_write_mask);
  key.push_back(requested_colors);
  key.push_back(used);
  key.push_back(targets.depth ? uint32_t(targets.depth->format) : 0u);
  if (used & kUsesDepth) {
    for (uint32_t value :
         {fixed.depth_enable, fixed.depth_function, fixed.depth_write_enable, fixed.stencil_enable,
          fixed.stencil_function, fixed.stencil_fail, fixed.stencil_depth_fail, fixed.stencil_pass,
          fixed.two_sided_stencil, fixed.ccw_stencil_function, fixed.ccw_stencil_fail,
          fixed.ccw_stencil_depth_fail, fixed.ccw_stencil_pass, fixed.stencil_mask,
          fixed.stencil_write_mask, uint32_t(fixed.depth_bias_enable),
          fixed.depth_bias_enable ? fixed.depth_bias_bits : 0u,
          fixed.depth_bias_enable ? fixed.slope_scaled_depth_bias_bits : 0u})
      key.push_back(value);
  }
  key.push_back(fixed.cull_mode);
  key.insert(key.end(), inputs.key.begin(), inputs.key.end());
  if (auto found = pipelines.find(key); found != pipelines.end()) return &found->second;
  ScopedTimer timer(timing.pipeline_ms);
  ++timing.new_pipelines;

  auto vertex_module = Module(vertex, false, error);
  wgpu::ShaderModule pixel_module;
  if (pixel) pixel_module = Module(*pixel, late, error);
  if (!vertex_module || (pixel && !pixel_module)) return nullptr;

  Pipeline result;
  result.texture_mask = texture_mask;
  result.cube_mask = cube_mask;
  result.sampler_mask = sampler_mask;
  std::vector<wgpu::BindGroupLayout> groups{uniform_layout};
  if (texture_mask | sampler_mask) {
    result.textures = TextureLayout(texture_mask, cube_mask, sampler_mask);
    groups.push_back(result.textures);
  }
  wgpu::PipelineLayoutDescriptor layout{};
  layout.bindGroupLayoutCount = groups.size();
  layout.bindGroupLayouts = groups.data();

  std::vector<wgpu::VertexBufferLayout> buffers = inputs.layouts;
  for (size_t i = 0; i < buffers.size(); ++i) {
    buffers[i].attributeCount = inputs.attributes[i].size();
    buffers[i].attributes = inputs.attributes[i].data();
  }
  wgpu::RenderPipelineDescriptor descriptor{};
  const std::string label = fmt::format("title {:016X}/{:016X}", vertex.hash,
                                        pixel ? pixel->hash : 0);
  descriptor.label = wgpu::StringView(label.data(), label.size());
  descriptor.layout = device.CreatePipelineLayout(&layout);
  descriptor.vertex.module = vertex_module;
  descriptor.vertex.entryPoint = "main";
  descriptor.vertex.bufferCount = buffers.size();
  descriptor.vertex.buffers = buffers.data();
  descriptor.primitive.topology = wgpu::PrimitiveTopology(topology);
  descriptor.primitive.stripIndexFormat = wgpu::IndexFormat(strip_format);
  const auto cull = DecodeNativeCullRasterState(fixed.cull_mode);
  descriptor.primitive.cullMode = cull.cull_face == NativeCullFace::kFront ? wgpu::CullMode::Front
                                  : cull.cull_face == NativeCullFace::kBack ? wgpu::CullMode::Back
                                                                            : wgpu::CullMode::None;
  descriptor.primitive.frontFace =
      cull.front_face_clockwise ? wgpu::FrontFace::CW : wgpu::FrontFace::CCW;

  wgpu::DepthStencilState depth{};
  if (targets.depth && !(used & kUsesDepth)) {
    // Attached for the pass's other draws; this one leaves it alone.
    depth.format = targets.depth->format;
    depth.depthWriteEnabled = wgpu::OptionalBool::False;
    depth.depthCompare = wgpu::CompareFunction::Always;
    depth.stencilFront = depth.stencilBack = {
        wgpu::CompareFunction::Always, wgpu::StencilOperation::Keep,
        wgpu::StencilOperation::Keep, wgpu::StencilOperation::Keep};
    depth.stencilReadMask = depth.stencilWriteMask = 0;
    descriptor.depthStencil = &depth;
  } else if (targets.depth) {
    depth.format = targets.depth->format;
    depth.depthWriteEnabled = fixed.depth_enable && fixed.depth_write_enable
                                  ? wgpu::OptionalBool::True
                                  : wgpu::OptionalBool::False;
    depth.depthCompare =
        fixed.depth_enable ? Compare(fixed.depth_function) : wgpu::CompareFunction::Always;
    if (fixed.stencil_enable) {
      depth.stencilFront = {Compare(fixed.stencil_function), StencilOp(fixed.stencil_fail),
                            StencilOp(fixed.stencil_depth_fail), StencilOp(fixed.stencil_pass)};
      depth.stencilBack =
          fixed.two_sided_stencil
              ? wgpu::StencilFaceState{Compare(fixed.ccw_stencil_function),
                                       StencilOp(fixed.ccw_stencil_fail),
                                       StencilOp(fixed.ccw_stencil_depth_fail),
                                       StencilOp(fixed.ccw_stencil_pass)}
              : depth.stencilFront;
      depth.stencilReadMask = fixed.stencil_mask;
      depth.stencilWriteMask = fixed.stencil_write_mask;
    } else {
      depth.stencilFront = depth.stencilBack = {
          wgpu::CompareFunction::Always, wgpu::StencilOperation::Keep,
          wgpu::StencilOperation::Keep, wgpu::StencilOperation::Keep};
      depth.stencilReadMask = depth.stencilWriteMask = 0;
    }
    if (fixed.depth_bias_enable) {
      depth.depthBias = int32_t(std::lround(std::bit_cast<float>(fixed.depth_bias_bits)));
      depth.depthBiasSlopeScale = std::bit_cast<float>(fixed.slope_scaled_depth_bias_bits);
    }
    descriptor.depthStencil = &depth;
  }

  std::array<wgpu::ColorTargetState, kRenderTargetCount> colors{};
  std::array<wgpu::BlendState, kRenderTargetCount> blends{};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    const auto& surface = targets.colors[i];
    if (!surface) continue;
    color_count = i + 1;
    colors[i].format = surface->format;
    const uint32_t write = (pixel && (used & (1u << i)))
                               ? NativeColorWriteMaskForTarget(fixed.color_write_mask, i)
                               : 0;
    colors[i].writeMask = wgpu::ColorWriteMask(write);
    if (!write || !IsNativeBlendControlEnabled(fixed.blend_controls[i]) ||
        !Blendable(surface->format, float32_blendable))
      continue;
    const auto blend = DecodeNativeBlendControl(fixed.blend_controls[i]);
    auto& state = blends[i];
    if (!BlendFactor(blend.source_color, state.color.srcFactor) ||
        !BlendFactor(blend.destination_color, state.color.dstFactor) ||
        !BlendFactor(blend.source_alpha, state.alpha.srcFactor) ||
        !BlendFactor(blend.destination_alpha, state.alpha.dstFactor) ||
        !BlendOperation(blend.color_operation, state.color.operation) ||
        !BlendOperation(blend.alpha_operation, state.alpha.operation)) {
      error = "Unsupported title blend state";
      return nullptr;
    }
    for (auto* component : {&state.color, &state.alpha}) {
      if (component->operation == wgpu::BlendOperation::Min ||
          component->operation == wgpu::BlendOperation::Max)
        component->srcFactor = component->dstFactor = wgpu::BlendFactor::One;
    }
    colors[i].blend = &state;
  }
  wgpu::FragmentState fragment{};
  if (pixel || color_count) {
    // Without a pixel shader, the pass's color attachments still need
    // (unwritten) targets.
    fragment.module = pixel ? pixel_module : UtilityModule("empty_fragment", "@fragment fn main() {}\n");
    fragment.entryPoint = "main";
    fragment.targetCount = color_count;
    fragment.targets = colors.data();
    descriptor.fragment = &fragment;
  }
  if (pipelines.size() >= 8192) {
    pipelines.clear();
    ++pipeline_generation;
    pass_state.pipeline = nullptr;  // A new pipeline could reuse the handle.
  }
  if (!REXCVAR_GET(webgpu_async_pipelines) || trace) {
    result.pipeline = device.CreateRenderPipeline(&descriptor);
    return &pipelines.emplace(key, std::move(result)).first->second;
  }
  // A synchronous creation stalls everything after it (in the browser, the
  // GPU process compiles it before later commands; under Node, this thread
  // waits), so draws using it are skipped until the callback brings it.
  Pipeline* entry = &pipelines.emplace(key, std::move(result)).first->second;
  device.CreateRenderPipelineAsync(
      &descriptor, wgpu::CallbackMode::AllowSpontaneous,
      [this, entry, generation = pipeline_generation, requested = emscripten_get_now(), label](
          wgpu::CreatePipelineAsyncStatus status, wgpu::RenderPipeline created,
          wgpu::StringView message) {
        // The cache was cleared since (or the renderer is shutting down).
        if (status == wgpu::CreatePipelineAsyncStatus::CallbackCancelled ||
            generation != pipeline_generation)
          return;
        const double latency = emscripten_get_now() - requested;
        timing.pipeline_latency_ms += latency;
        timing.pipeline_latency_max_ms = std::max(timing.pipeline_latency_max_ms, latency);
        if (status == wgpu::CreatePipelineAsyncStatus::Success && created) {
          entry->pipeline = std::move(created);
          ++timing.pipelines_ready;
          return;
        }
        entry->failed = true;
        if (++timing.pipelines_failed <= 32)
          REXLOG_ERROR("gta4-webgpu: pipeline {} creation failed ({}): {}", label, uint32_t(status),
                       std::string_view(message.data ? message.data : "",
                                        message.data && message.length != WGPU_STRLEN
                                            ? message.length
                                            : message.data ? std::strlen(message.data) : 0));
      });
  return entry;
}

const InputLayout* Renderer::State::Inputs(const ShaderRecord& vertex, uint32_t declaration,
                                           const std::vector<VertexElement>& elements, bool up,
                                           uint32_t bound_streams, std::string& error) {
  const uint64_t record = reinterpret_cast<uintptr_t>(&vertex);
  const InputKey input_key{uint32_t(record), uint32_t(record >> 32), declaration,
                           up ? 0x80000000u : bound_streams};
  if (auto found = input_layouts.find(input_key); found != input_layouts.end())
    return &found->second;
  InputLayout layout;
  std::vector<uint32_t> defaults;
  for (uint32_t location = 0; location < vertex.attributes.size(); ++location) {
    const uint32_t semantic = vertex.attributes[location];
    const VertexElement* match = nullptr;
    for (const auto& element : elements) {
      if (core::ConvertVertexUsageToLocation(element.usage, element.usage_index) != semantic)
        continue;
      if (up ? element.stream == 0 : (bound_streams & (1u << element.stream)) != 0) {
        match = &element;
        break;
      }
    }
    if (!match) {
      defaults.push_back(location);
      continue;
    }
    auto group = std::find_if(layout.streams.begin(), layout.streams.end(),
                              [&](const auto& item) { return item.stream == match->stream; });
    if (group == layout.streams.end()) {
      layout.streams.push_back({match->stream, {}, {}});
      group = std::prev(layout.streams.end());
    }
    group->locations.push_back(location);
    group->elements.push_back(match);
  }
  if (layout.streams.size() + (defaults.empty() ? 0 : 1) > limits.maxVertexBuffers) {
    error = "Draw uses more vertex streams than WebGPU allows";
    return nullptr;
  }
  for (const auto& input : layout.streams) {
    wgpu::VertexBufferLayout buffer{};
    buffer.arrayStride = input.elements.size() * 16;
    buffer.stepMode = wgpu::VertexStepMode::Vertex;
    layout.layouts.push_back(buffer);
    std::vector<wgpu::VertexAttribute> list;
    for (size_t i = 0; i < input.locations.size(); ++i)
      list.push_back({nullptr, wgpu::VertexFormat::Float32x4, uint64_t(i * 16), input.locations[i]});
    layout.attributes.push_back(std::move(list));
  }
  if (!defaults.empty()) {
    layout.defaults = true;
    wgpu::VertexBufferLayout buffer{};
    buffer.arrayStride = 0;
    buffer.stepMode = wgpu::VertexStepMode::Vertex;
    layout.layouts.push_back(buffer);
    std::vector<wgpu::VertexAttribute> list;
    for (uint32_t location : defaults)
      list.push_back({nullptr, wgpu::VertexFormat::Float32x4, 0, location});
    layout.attributes.push_back(std::move(list));
  }
  for (size_t i = 0; i < layout.layouts.size(); ++i) {
    layout.key.push_back(uint32_t(layout.layouts[i].arrayStride));
    for (const auto& attribute : layout.attributes[i]) {
      layout.key.push_back(attribute.shaderLocation);
      layout.key.push_back(uint32_t(attribute.offset));
    }
    layout.key.push_back(0xFFFFFFFFu);
  }
  return &input_layouts.emplace(input_key, std::move(layout)).first->second;
}

bool Renderer::State::Draw(const Work& work, std::string& error, bool retry) {
  const uint64_t batch = batches;
  uint32_t type = 0, first = 0, count = 0, up_stride = 0, restart_value = 0;
  int32_t base_vertex = 0;
  bool indexed = false, restart = false, up = false;
  switch (work.type()) {
    case CommandType::kDrawPrimitive: {
      const auto c = work.As<DrawPrimitiveCommand>();
      type = c.primitive_type;
      first = c.start_vertex;
      count = c.vertex_count;
      break;
    }
    case CommandType::kDrawPrimitiveUp: {
      const auto c = work.As<DrawPrimitiveUpCommand>();
      up = true;
      type = c.primitive_type;
      count = c.vertex_count;
      up_stride = c.stride;
      break;
    }
    default: {
      const auto c = work.As<DrawIndexedPrimitiveCommand>();
      indexed = true;
      type = c.primitive_type;
      first = c.start_index;
      count = c.index_count;
      base_vertex = c.base_vertex;
      restart = c.primitive_restart_enabled;
      restart_value = c.primitive_restart_index;
      break;
    }
  }
  if (!count) return true;
  const uint32_t vertex_count = count;  // Before fan/quad index conversion.
  const auto device_found = devices.find(work.device.device);
  if (count > 16u * 1024u * 1024u || device_found == devices.end()) {
    error = "Draw range exceeds its bound or has no device state";
    return false;
  }
  const std::span<const uint8_t> guest(device_found->second);
  const auto fixed = core::DecodeFixedState(guest);

  const auto vertex_found = shaders.find(vertex_shader);
  if (vertex_found == shaders.end()) {
    error = fmt::format("Draw references an unregistered vertex shader {:08X}", vertex_shader);
    return false;
  }
  const ShaderRecord& vertex = *vertex_found->second;
  const ShaderRecord* pixel = nullptr;
  if (pixel_shader) {
    const auto found = shaders.find(pixel_shader);
    if (found == shaders.end()) {
      error = fmt::format("Draw references an unregistered pixel shader {:08X}", pixel_shader);
      return false;
    }
    pixel = found->second;
    static const std::vector<uint64_t> skipped = [] {
      std::vector<uint64_t> hashes;
      const std::string list = REXCVAR_GET(webgpu_skip_pixel_shader);
      for (const char* p = list.c_str(); *p;) {
        char* end = nullptr;
        if (const uint64_t hash = std::strtoull(p, &end, 16); end != p) hashes.push_back(hash);
        p = *end ? end + 1 : end;
      }
      return hashes;
    }();
    if (std::find(skipped.begin(), skipped.end(), pixel->hash) != skipped.end()) return true;
  }
  const auto declaration_found = declarations.find(declaration);
  if (declaration_found == declarations.end()) {
    error = "Draw has no registered vertex declaration";
    return false;
  }
  const auto& elements = declaration_found->second;

  const bool alpha = fixed.alpha_test_enable && fixed.alpha_function != 7;
  const bool coverage = alpha || IsNativeAlphaToMaskRequested(fixed.alpha_to_mask);
  const bool late = coverage && pixel && !pixel->late.empty();
  const uint32_t specialization = alpha ? 2u | ((fixed.alpha_function & 7u) << 8u) : 0u;

  // Attachments.
  const uint32_t requested_colors = NativeColorTargetMaskFromWriteMask(fixed.color_write_mask) &
                                    (pixel ? pixel->color_output_mask : 0u);
  Targets targets;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    if (!(requested_colors & (1u << i)) || !work.colors[i].handle) continue;
    targets.colors[i] = Surface(work.colors[i], false, error);
    if (!targets.colors[i]) return false;
  }
  if ((fixed.depth_enable || fixed.stencil_enable) && work.depth.handle) {
    targets.depth = Surface(work.depth, true, error);
    if (!targets.depth) return false;
  }
  for (const auto* surface : {targets.colors[0].get(), targets.colors[1].get(),
                              targets.colors[2].get(), targets.colors[3].get(), targets.depth.get()}) {
    if (!surface) continue;
    if (!targets.width) {
      targets.width = surface->width;
      targets.height = surface->height;
    } else if (surface->width != targets.width || surface->height != targets.height) {
      error = "Title attachments have different sizes";
      return false;
    }
  }
  if (targets.empty()) {  // Nothing this draw can write.
    ++stats.no_targets;
    return true;
  }
  uint32_t used = targets.depth ? kUsesDepth : 0u;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i)
    if (targets.colors[i]) used |= 1u << i;
  // The pass's attachments: those of the open pass when they hold this
  // draw's, so that toggling depth or a color target does not end it (each
  // new pass also has every binding set again); otherwise this draw's plus
  // the other bound surfaces of the same size, for the draws that follow.
  // A copy: a flush during the setup below ends the pass.
  Targets attachments;
  if (pass && pass_targets.Holds(targets)) {
    attachments = pass_targets;
  } else {
    attachments = targets;
    // Only surfaces that exist already and match their binding: making or
    // replacing one is left to a draw that uses it.
    const auto add = [&](std::shared_ptr<SurfaceResource>& slot,
                         const SurfaceDescriptor& descriptor, bool depth) {
      if (slot || !descriptor.handle) return;
      const auto found = surfaces.find(descriptor.handle);
      if (found == surfaces.end()) return;
      const auto& surface = found->second;
      if (surface->depth != depth || surface->width != targets.width ||
          surface->height != targets.height || descriptor.width != surface->width ||
          descriptor.height != surface->height ||
          surface->format != SurfaceFormat(descriptor.format, depth, depth_format))
        return;
      for (const auto& other : attachments.colors)
        if (other == surface) return;
      slot = surface;
    };
    for (uint32_t i = 0; i < kRenderTargetCount; ++i)
      add(attachments.colors[i], work.colors[i], false);
    add(attachments.depth, work.depth, true);
  }

  std::array<float, 6> viewport{};
  for (size_t i = 0; i < viewport.size(); ++i) {
    viewport[i] = std::bit_cast<float>(fixed.viewport_bits[i]);
    if (!std::isfinite(viewport[i])) {
      error = "Nonfinite viewport";
      return false;
    }
  }
  if (viewport[2] <= 0 || viewport[3] <= 0) {
    ++stats.empty_viewport;
    return true;
  }
  // WebGPU requires minDepth <= maxDepth; Xenos allows a reversed range.
  viewport[4] = std::clamp(viewport[4], 0.0f, 1.0f);
  viewport[5] = std::clamp(viewport[5], 0.0f, 1.0f);
  if (viewport[4] > viewport[5]) std::swap(viewport[4], viewport[5]);
  const int32_t left = std::clamp(fixed.scissor[0], 0, int32_t(targets.width));
  const int32_t top = std::clamp(fixed.scissor[1], 0, int32_t(targets.height));
  const int32_t right = std::clamp(fixed.scissor[2], 0, int32_t(targets.width));
  const int32_t bottom = std::clamp(fixed.scissor[3], 0, int32_t(targets.height));
  if (right <= left || bottom <= top) {
    ++stats.empty_scissor;
    return true;
  }

  // Room for this draw's uniform slots first: a flush between pushing its
  // geometry (below) and choosing its slots would submit them with the old
  // batch, and flushing later forgets slots it already chose.
  if (!UniformSpace(kUniformVertexBytes + kUniformPixelBytes + 1536, error)) return false;

  // Topology and index conversion (fans, quads and restart strips become
  // 32-bit lists or strips, as in the Metal renderer).
  wgpu::PrimitiveTopology topology;
  switch (type) {
    case 1: topology = wgpu::PrimitiveTopology::PointList; break;
    case 2: topology = wgpu::PrimitiveTopology::LineList; break;
    case 3: topology = wgpu::PrimitiveTopology::LineStrip; break;
    case 4: case 5: case 13: topology = wgpu::PrimitiveTopology::TriangleList; break;
    case 6: topology = wgpu::PrimitiveTopology::TriangleStrip; break;
    case 8:
      topology = up ? wgpu::PrimitiveTopology::TriangleList : wgpu::PrimitiveTopology::TriangleStrip;
      break;
    default:
      error = fmt::format("Unsupported title primitive topology {}", type);
      return false;
  }
  wgpu::Buffer index_buffer;
  uint64_t index_offset = 0;
  bool index32 = false;
  if (indexed) {
    if (!work.indices) {
      error = "Indexed draw has no captured index buffer";
      return false;
    }
    uint64_t index_base = 0;
    index_buffer = IndexBuffer(*work.indices, index32, index_base, error);
    if (!index_buffer) return false;
    const size_t element = index32 ? 4 : 2;
    if (uint64_t(first) + count > work.indices->bytes.size() / element) {
      error = "Index draw exceeds its captured buffer";
      return false;
    }
    index_offset = index_base + uint64_t(first) * element;
  }
  const bool strip = type == 3 || type == 6 || (type == 8 && !up);
  if (type == 5 || type == 13 || (strip && restart)) {
    if (type == 13 && count % 4) {
      error = "Incomplete guest quad list";
      return false;
    }
    const auto read = [&](uint32_t i) -> uint32_t {
      if (!indexed) return first + i;
      const uint8_t* bytes = work.indices->bytes.data();
      if (index32) return GuestWord(bytes, (size_t(first) + i) * 4) & 0x00FFFFFFu;
      return uint32_t(bytes[(size_t(first) + i) * 2]) << 8 | bytes[(size_t(first) + i) * 2 + 1];
    };
    const auto is_restart = [&](uint32_t value) { return indexed && restart && value == restart_value; };
    auto& converted = index_scratch;
    converted.clear();
    if (type == 5) {
      VisitNativeTriangleFan(count, read, is_restart, [&](uint32_t a, uint32_t b, uint32_t c) {
        converted.insert(converted.end(), {a, b, c});
      });
    } else if (type == 13) {
      converted.reserve(size_t(count) / 4 * 6);
      for (uint32_t i = 0; i < count; i += 4) {
        const uint32_t a = read(i), b = read(i + 1), c = read(i + 2), d = read(i + 3);
        converted.insert(converted.end(), {a, b, c, a, c, d});
      }
    } else {
      converted.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t value = read(i);
        converted.push_back(is_restart(value) ? UINT32_MAX : value);
      }
    }
    if (converted.empty()) return true;
    index_offset = PushGeometry({reinterpret_cast<const uint8_t*>(converted.data()),
                                 converted.size() * sizeof(uint32_t)},
                                error);
    if (index_offset == UINT64_MAX) return false;
    index_buffer = geometry.buffer;
    indexed = index32 = true;
    count = uint32_t(converted.size());
    first = 0;
  }
  const uint32_t strip_format =
      strip && indexed ? uint32_t(index32 ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16)
                       : uint32_t(wgpu::IndexFormat::Undefined);

  // Vertex inputs: one float4 per shader attribute, grouped by guest stream.
  std::optional<ScopedTimer> inputs_timer(std::in_place, timing.inputs_ms);
  uint32_t bound_streams = 0;
  if (!up) {
    for (uint32_t i = 0; i < kVertexStreamCount; ++i)
      if (work.streams[i] && streams[i].stride) bound_streams |= 1u << i;
  }
  const InputLayout* inputs = Inputs(vertex, declaration, elements, up, bound_streams, error);
  if (!inputs) return false;
  auto& bindings = vertex_bindings;
  bindings.clear();
  for (const auto& input : inputs->streams) {
    VertexBinding binding;
    if (up) {
      if (!up_stride || uint64_t(up_stride) * vertex_count > work.up_vertices.size()) {
        error = "Invalid UP vertex payload extent";
        return false;
      }
      const size_t components = input.elements.size() * 4;
      auto& decoded = up_scratch;
      decoded.assign(size_t(vertex_count) * components, 0.0f);
      for (uint32_t v = 0; v < vertex_count; ++v) {
        for (size_t i = 0; i < input.elements.size(); ++i) {
          const auto* element = input.elements[i];
          float* out = decoded.data() + v * components + i * 4;
          if (element->offset + VertexElementSize(element->type) > up_stride ||
              !DecodeVertexElement(element->type,
                                   work.up_vertices.data() + size_t(v) * up_stride + element->offset,
                                   out))
            out[0] = out[1] = out[2] = out[3] = 0.0f;
        }
      }
      if (type == 8) {
        // Rectangle list: three corners per rectangle; reconstruct the fourth
        // opposite the longest edge and emit two triangles.
        if (vertex_count % 3) {
          error = "Incomplete rectangle list";
          return false;
        }
        const auto position = std::find(vertex.attributes.begin(), vertex.attributes.end(), 0);
        size_t position_index = SIZE_MAX;
        if (position != vertex.attributes.end()) {
          const auto at = std::find(input.locations.begin(), input.locations.end(),
                                    uint32_t(position - vertex.attributes.begin()));
          if (at != input.locations.end()) position_index = size_t(at - input.locations.begin());
        }
        auto& expanded = up_expanded;
        expanded.assign(decoded.size() * 2, 0.0f);
        for (uint32_t r = 0; r < vertex_count / 3; ++r) {
          const float* corner[3] = {decoded.data() + (r * 3) * components,
                                    decoded.data() + (r * 3 + 1) * components,
                                    decoded.data() + (r * 3 + 2) * components};
          uint32_t start = 0;
          if (position_index != SIZE_MAX) {
            float longest = -1;
            for (uint32_t k = 0; k < 3; ++k) {
              const float* a = corner[(k + 1) % 3] + position_index * 4;
              const float* b = corner[(k + 2) % 3] + position_index * 4;
              const float length = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]);
              if (length > longest) {
                longest = length;
                start = k;
              }
            }
          }
          const float* a = corner[start];
          const float* b = corner[(start + 1) % 3];
          const float* c = corner[(start + 2) % 3];
          float* out = expanded.data() + size_t(r) * 6 * components;
          const float* order[5] = {a, b, c, c, b};
          for (size_t k = 0; k < 5; ++k)
            std::memcpy(out + k * components, order[k], components * sizeof(float));
          for (size_t k = 0; k < components; ++k) out[5 * components + k] = b[k] - a[k] + c[k];
        }
        decoded.swap(expanded);
      }
      binding.offset = PushGeometry({reinterpret_cast<const uint8_t*>(decoded.data()),
                                     decoded.size() * sizeof(float)},
                                    error, components * sizeof(float));
      if (binding.offset == UINT64_MAX) return false;
      binding.buffer = geometry.buffer;
      binding.size = decoded.size() * sizeof(float);
    } else {
      if (!VertexBuffer(*work.streams[input.stream], streams[input.stream].offset,
                        streams[input.stream].stride, input.elements, binding, error))
        return false;
    }
    bindings.push_back(binding);
  }
  // Every stream's offset is a multiple of its stride. When they all start
  // the same number of vertices in, bind the buffers from their start and
  // select the vertices with baseVertex/firstVertex instead, so draws from
  // the same pooled buffer keep its binding.
  uint32_t vertex_shift = 0;
  if (!bindings.empty()) {
    bool shared = true;
    const uint64_t shift = bindings[0].offset / inputs->layouts[0].arrayStride;
    for (size_t i = 1; i < bindings.size(); ++i)
      shared = shared && bindings[i].offset / inputs->layouts[i].arrayStride == shift;
    if (shared && shift <= uint64_t(INT32_MAX / 2)) {
      vertex_shift = uint32_t(shift);
      for (auto& binding : bindings) {
        binding.offset = 0;
        binding.size = wgpu::kWholeSize;
      }
    }
  }
  if (inputs->defaults) bindings.push_back({zero_vertices, 0, 64});
  if (up && type == 8) count *= 2;
  inputs_timer.reset();

  auto* pipeline = DrawPipeline(attachments, fixed, vertex, pixel, late, specialization,
                                uint32_t(topology), strip_format, *inputs, requested_colors, used,
                                error);
  if (!pipeline) return false;
  if (!pipeline->pipeline) {
    if (pipeline->failed) {
      error = "Title pipeline creation failed";
      return false;
    }
    ++stats.waiting;
    ++timing.waiting_draws;
    return true;
  }

  // Shared constants, then textures.
  core::SharedConstants shared{};
  wgpu::BindGroup texture_group;
  std::optional<ScopedTimer> bind_timer(std::in_place, timing.bind_ms);
  if (pipeline->textures) {
    group_key.clear();
    const auto key_pointer = [&](const void* pointer) {
      const uint64_t value = reinterpret_cast<uintptr_t>(pointer);
      group_key.push_back(uint32_t(value));
      group_key.push_back(uint32_t(value >> 32));
    };
    key_pointer(pipeline->textures.Get());
    auto& entries = group_entries;
    entries.clear();
    for (uint32_t slot = 0; slot < kTextureStageCount; ++slot) {
      const uint32_t bit = 1u << slot;
      if (!((pipeline->texture_mask | pipeline->sampler_mask) & bit)) continue;
      const uint32_t handle = GuestWord(guest.data(), kDeviceTextures + slot * 4);
      xenos::xe_gpu_texture_fetch_t fetch{};
      auto* words = reinterpret_cast<uint32_t*>(&fetch);
      for (size_t word = 0; word < 6; ++word)
        words[word] = GuestWord(guest.data(), kDeviceFetchConstants + slot * 0x18 + word * 4);
      shared.sampler_lod_bias[slot] = float(fetch.lod_bias) / 32.0f;
      std::shared_ptr<TextureResource> resource;
      if (handle) {
        std::string texture_error;
        if (const auto alias = packed_depth_aliases.find(handle); alias != packed_depth_aliases.end())
          resource = PackedDepthAlias(handle, alias->second, fetch, texture_error);
        else
          resource = SampledTexture(handle, fetch, work.textures[slot], texture_error);
        if (trace || (!resource && !texture_error.empty() && ++texture_failures <= 32)) {
          size_t bytes = 0, nonzero = 0;
          if (const auto& capture = work.textures[slot]; capture && !capture->mips.empty()) {
            bytes = capture->mips[0].bytes.size();
            nonzero = size_t(std::count_if(capture->mips[0].bytes.begin(),
                                           capture->mips[0].bytes.end(),
                                           [](uint8_t value) { return value != 0; }));
          }
          REXLOG_INFO("webgpu-texture: slot={} handle={:08X} captured={} resource={} {}x{} "
                      "format={} gpu={} guest-format={} mip0={}/{} nonzero error={}",
                      slot, handle, bool(work.textures[slot]), bool(resource),
                      resource ? resource->width : 0, resource ? resource->height : 0,
                      resource ? uint32_t(resource->format) : 0,
                      resource && resource->gpu_produced,
                      resource ? uint32_t(resource->info.format) : 0, nonzero, bytes,
                      texture_error);
        }
      }
      if (pipeline->texture_mask & bit) {
        const bool cube = pipeline->cube_mask & bit;
        wgpu::TextureView view = cube ? fallback_cube_view : fallback_2d_view;
        if (resource) {
          if (cube == (resource->dimension == wgpu::TextureViewDimension::Cube) &&
              resource->dimension != wgpu::TextureViewDimension::e3D) {
            if (resource->dimension == wgpu::TextureViewDimension::e2DArray) {
              if (!resource->first_layer) {
                wgpu::TextureViewDescriptor layer{};
                layer.dimension = wgpu::TextureViewDimension::e2D;
                layer.arrayLayerCount = 1;
                resource->first_layer = resource->texture.CreateView(&layer);
              }
              view = resource->first_layer;
            } else {
              view = resource->view;
            }
          }
        }
        wgpu::BindGroupEntry entry{};
        entry.binding = slot;
        entry.textureView = view;
        entries.push_back(entry);
        key_pointer(view.Get());
      }
      if (pipeline->sampler_mask & bit) {
        wgpu::BindGroupEntry entry{};
        entry.binding = kSamplerBindingBase + slot;
        entry.sampler = Sampler(fetch, resource.get());
        entries.push_back(entry);
        key_pointer(entry.sampler.Get());
      }
    }
    if (auto found = texture_groups.find(group_key); found != texture_groups.end()) {
      texture_group = found->second.first;
      found->second.second = frame;
    } else {
      wgpu::BindGroupDescriptor descriptor{};
      descriptor.layout = pipeline->textures;
      descriptor.entryCount = entries.size();
      descriptor.entries = entries.data();
      texture_group = device.CreateBindGroup(&descriptor);
      // The cached group holds its views and samplers, so the pointers in
      // its key cannot be reused by other objects while it lives.
      texture_groups.emplace(group_key, std::make_pair(texture_group, frame));
      ++timing.new_groups;
    }
  }
  bind_timer.reset();
  for (uint32_t i = 0; i < kRenderTargetCount; ++i)
    shared.color_output[i] =
        NativeColorOutput(work.colors[i].address, targets.colors[i] && (requested_colors & (1u << i)));
  shared.booleans = PackNativeShaderBooleans(GuestWord(guest.data(), 0x2780),
                                             GuestWord(guest.data(), 0x2790));
  shared.half_pixel_offset_x = 1.0f / float(targets.width);
  shared.half_pixel_offset_y = -1.0f / float(targets.height);
  shared.fragment_coordinate_scale_x = 1.0f;
  shared.fragment_coordinate_scale_y = 1.0f;
  for (size_t i = 0; i < 4; ++i) shared.clip_plane[i] = std::bit_cast<float>(fixed.clip_plane_bits[i]);
  shared.clip_plane_enabled = (fixed.user_clip_plane_enable_mask & 1u) != 0;
  shared.alpha_threshold = fixed.alpha_reference;
  shared.alpha_to_mask = fixed.alpha_to_mask;
  shared.alpha_to_mask_sample_count = 1;
  shared.motion_blur_time_scale = 1.0f;
  shared.modern_effects = BuildModernEffectConstants(false, 0, &environment);
  for (size_t i = 0; i < 4; ++i) shared.modern_effects.viewport[i] = viewport[i];
  shared.modern_effects.depth_range[0] = std::clamp(viewport[4], 0.0f, 1.0f);
  shared.modern_effects.depth_range[1] = std::clamp(viewport[5], 0.0f, 1.0f);
  shared.environmental_valid_fields = environment.valid_fields;
  shared.fog_parameters[0] = environment.fog_density;
  shared.fog_parameters[1] = environment.fog_height_falloff;
  shared.fog_parameters[2] = environment.fog_altitude_tweak;
  shared.fog_parameters[3] = environment.fog_power;
  std::copy(environment.camera_position.begin(), environment.camera_position.end(),
            shared.camera_position);
  std::copy(environment.view_inverse_matrix.begin(), environment.view_inverse_matrix.end(),
            shared.view_inverse_matrix);
  shared.projection_scale[0] = environment.projection_matrix[0];
  shared.projection_scale[1] = environment.projection_matrix[5];

  // Each part of the constants keeps its slot while its inputs are unchanged.
  std::optional<ScopedTimer> uniform_timer(std::in_place, timing.uniform_ms);
  auto& last = last_uniforms;
  if (last.device != work.device.device) {
    last.vertex = last.pixel = UINT64_MAX;
    last.device = work.device.device;
  }
  if (last.vertex == UINT64_MAX || last.vertex_serial != vertex_constants_serial) {
    uint8_t* slot = ReserveUniforms(kUniformVertexBytes, last.vertex, error);
    if (!slot) return false;
    CopySwap32(slot, guest.data() + kDeviceVertexConstants, kUniformVertexBytes);
    last.vertex_serial = vertex_constants_serial;
    ++timing.vertex_slots;
  }
  if (pixel && (last.pixel == UINT64_MAX || last.pixel_serial != pixel_constants_serial)) {
    uint8_t* slot = ReserveUniforms(kUniformPixelBytes, last.pixel, error);
    if (!slot) return false;
    CopySwap32(slot, guest.data() + kDevicePixelConstants, kPixelConstantBytes);
    std::memset(slot + kPixelConstantBytes, 0, kUniformPixelBytes - kPixelConstantBytes);
    last.pixel_serial = pixel_constants_serial;
    ++timing.pixel_slots;
  }
  if (last.shared == UINT64_MAX || last.specialization != specialization ||
      !BytesEqual(&last.constants, &shared, sizeof(shared))) {
    uint8_t* slot = ReserveUniforms(kUniformSharedBytes, last.shared, error);
    if (!slot) return false;
    std::memcpy(slot, &shared, sizeof(shared));
    std::memset(slot + sizeof(shared), 0, kUniformSharedBytes - sizeof(shared));
    std::memcpy(slot + kUniformSpecializationOffset, &specialization, 4);
    last.specialization = specialization;
    last.constants = shared;
    ++timing.shared_slots;
  }
  // Without a pixel shader the pixel binding is unread; any slot will do.
  const std::array<uint32_t, kUniformBindings> uniform_offsets{
      uint32_t(last.vertex), uint32_t(pixel ? last.pixel : last.vertex), uint32_t(last.shared)};
  uniform_timer.reset();

  if (trace) {
    float position[4] = {};
    if (!inputs->streams.empty() && !up && work.streams[inputs->streams[0].stream])
      DecodeVertexElement(inputs->streams[0].elements[0]->type,
                          work.streams[inputs->streams[0].stream]->bytes.data() +
                              streams[inputs->streams[0].stream].offset + inputs->streams[0].elements[0]->offset,
                          position);
    float c0[4];
    std::memcpy(c0, uniforms.bytes.data() + last_uniforms.vertex + 208 * 16, sizeof(c0));
    if (up && !work.up_vertices.empty() && !inputs->streams.empty())
      DecodeVertexElement(inputs->streams[0].elements[0]->type,
                          work.up_vertices.data() + inputs->streams[0].elements[0]->offset, position);
    REXLOG_INFO("webgpu-trace: draw vs={:016X} ps={:016X} type={} count={} indexed={} up={} "
                "color0={:08X}/{}x{} depth={:08X} viewport={},{},{},{},{},{} scissor={},{},{},{} "
                "writes={:X} blend0={:08X} depth-test={}/{} stencil={}/{}/{:02X}/{:02X}/{:02X} "
                "ops={}{}{} attr0={},{},{},{} c208={},{},{},{} textures={:X}",
                vertex.hash, pixel ? pixel->hash : 0, type, vertex_count, indexed, up,
                targets.colors[0] ? targets.colors[0]->descriptor.handle : 0,
                targets.colors[0] ? targets.colors[0]->width : 0,
                targets.colors[0] ? targets.colors[0]->height : 0,
                targets.depth ? targets.depth->descriptor.handle : 0, viewport[0], viewport[1],
                viewport[2], viewport[3], viewport[4], viewport[5], left, top, right, bottom,
                fixed.color_write_mask, fixed.blend_controls[0], fixed.depth_enable,
                fixed.depth_function, fixed.stencil_enable, fixed.stencil_function,
                fixed.stencil_reference, fixed.stencil_mask, fixed.stencil_write_mask,
                fixed.stencil_fail, fixed.stencil_depth_fail, fixed.stencil_pass, position[0],
                position[1], position[2], position[3], c0[0],
                c0[1], c0[2], c0[3], pipeline->texture_mask);
    // The first vertex's decoded inputs, and the nonzero pixel constants.
    std::string detail;
    for (const auto& input : inputs->streams) {
      const uint8_t* base =
          up ? work.up_vertices.data()
             : work.streams[input.stream] ? work.streams[input.stream]->bytes.data() +
                                                streams[input.stream].offset
                                          : nullptr;
      if (!base) continue;
      for (uint32_t v = 0; v < (up ? std::min(vertex_count, 6u) : 1u); ++v) {
        for (size_t i = 0; i < input.elements.size(); ++i) {
          float value[4];
          DecodeVertexElement(input.elements[i]->type,
                              base + size_t(v) * up_stride + input.elements[i]->offset, value);
          detail += fmt::format(" v{}in{}({:X})={},{},{},{}", v, input.locations[i],
                                input.elements[i]->type, value[0], value[1], value[2], value[3]);
        }
      }
    }
    if (pixel) {
      for (uint32_t i = 0; i < kPixelConstantBytes / 16; ++i) {
        float value[4];
        std::memcpy(value, uniforms.bytes.data() + last_uniforms.pixel + i * 16, sizeof(value));
        if (value[0] || value[1] || value[2] || value[3])
          detail += fmt::format(" c{}={},{},{},{}", i, value[0], value[1], value[2], value[3]);
      }
    }
    REXLOG_INFO("webgpu-trace: inputs{}", detail);
  }
  if (batches != batch) {
    if (retry) error = "Draw setup flushed its batch twice";
    else ++timing.redrawn;
    return false;
  }
  if (!BeginPass(attachments, error)) return false;
  // Every pass call crosses from wasm into the browser's WebGPU, so state that
  // the previous draw in this pass already set is skipped.
  ScopedTimer encode_timer(timing.encode_ms);
  auto& bound = pass_state;
  if (bound.pipeline != pipeline->pipeline.Get()) {
    pass.SetPipeline(pipeline->pipeline);
    bound.pipeline = pipeline->pipeline.Get();
    ++timing.set_pipeline;
  }
  if (!bound.uniforms_set || bound.uniform_offsets != uniform_offsets) {
    pass.SetBindGroup(0, uniform_group, uniform_offsets.size(), uniform_offsets.data());
    ++timing.set_group;
    bound.uniforms_set = true;
    bound.uniform_offsets = uniform_offsets;
  }
  if (texture_group && bound.textures != texture_group.Get()) {
    pass.SetBindGroup(1, texture_group);
    ++timing.set_group;
    bound.textures = texture_group.Get();
  }
  for (uint32_t i = 0; i < bindings.size(); ++i) {
    auto& slot = bound.vertex_buffers[i];
    if (slot.buffer == bindings[i].buffer.Get() && slot.offset == bindings[i].offset &&
        slot.size == bindings[i].size)
      continue;
    pass.SetVertexBuffer(i, bindings[i].buffer, bindings[i].offset, bindings[i].size);
    ++timing.set_vertex;
    slot = {bindings[i].buffer.Get(), bindings[i].offset, bindings[i].size};
  }
  const std::array<float, 6> viewport_state{viewport[0], viewport[1], viewport[2], viewport[3],
                                            std::clamp(viewport[4], 0.0f, 1.0f),
                                            std::clamp(viewport[5], 0.0f, 1.0f)};
  if (!bound.viewport_set || bound.viewport != viewport_state) {
    pass.SetViewport(viewport_state[0], viewport_state[1], viewport_state[2], viewport_state[3],
                     viewport_state[4], viewport_state[5]);
    bound.viewport = viewport_state;
    bound.viewport_set = true;
    ++timing.set_state;
  }
  const std::array<uint32_t, 4> scissor{uint32_t(left), uint32_t(top), uint32_t(right - left),
                                        uint32_t(bottom - top)};
  if (!bound.scissor_set || bound.scissor != scissor) {
    pass.SetScissorRect(scissor[0], scissor[1], scissor[2], scissor[3]);
    bound.scissor = scissor;
    bound.scissor_set = true;
    ++timing.set_state;
  }
  if (!bound.stencil_set || bound.stencil_reference != fixed.stencil_reference) {
    pass.SetStencilReference(fixed.stencil_reference);
    bound.stencil_reference = fixed.stencil_reference;
    bound.stencil_set = true;
    ++timing.set_state;
  }
  const std::array<float, 4> blend_state{fixed.blend_constants[0], fixed.blend_constants[1],
                                         fixed.blend_constants[2], fixed.blend_constants[3]};
  if (!bound.blend_set || bound.blend != blend_state) {
    const wgpu::Color blend{blend_state[0], blend_state[1], blend_state[2], blend_state[3]};
    pass.SetBlendConstant(&blend);
    bound.blend = blend_state;
    bound.blend_set = true;
    ++timing.set_state;
  }
  if (indexed) {
    // Bind the whole index buffer and select the range with firstIndex, so
    // draws from the same buffer share one binding.
    const uint32_t element = index32 ? 4 : 2;
    const auto format = index32 ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16;
    if (bound.index_buffer != index_buffer.Get() || bound.index_format != format) {
      pass.SetIndexBuffer(index_buffer, format, 0, wgpu::kWholeSize);
      ++timing.set_index;
      bound.index_buffer = index_buffer.Get();
      bound.index_format = format;
    }
    pass.DrawIndexed(count, 1, uint32_t(index_offset / element),
                     base_vertex + int32_t(vertex_shift), 0);
  } else {
    pass.Draw(count, 1, (up ? 0 : first) + vertex_shift, 0);
  }
  for (uint32_t i = 0; i < kRenderTargetCount; ++i)
    if (targets.colors[i]) targets.colors[i]->content_serial = ++content_serial;
  if (targets.depth && ((fixed.depth_enable && fixed.depth_write_enable) || fixed.stencil_enable))
    targets.depth->content_serial = ++content_serial;
  ++draws;
  ++frame_draws;
  ++stats.draws;
  if (trace) ProbePixel(targets, pixel ? pixel->hash : 0);
  return true;
}

void Renderer::State::ProbePixel(const Targets& targets, uint64_t pixel_shader) {
  constexpr size_t kMaximumProbes = 4096, kProbeStride = 256;
  static const auto position = [] {
    std::array<uint32_t, 2> xy{UINT32_MAX, UINT32_MAX};
    std::sscanf(REXCVAR_GET(webgpu_trace_pixel).c_str(), "%u,%u", &xy[0], &xy[1]);
    return xy;
  }();
  const auto& color = targets.colors[0];
  if (!color || position[0] >= color->width || position[1] >= color->height ||
      probes.size() >= kMaximumProbes)
    return;
  if (!probe_buffer) {
    wgpu::BufferDescriptor descriptor{};
    descriptor.size = kMaximumProbes * kProbeStride;
    descriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    probe_buffer = device.CreateBuffer(&descriptor);
  }
  EndPass();
  wgpu::TexelCopyTextureInfo from{};
  from.texture = color->texture;
  from.origin = {position[0], position[1], 0};
  wgpu::TexelCopyBufferInfo to{};
  to.buffer = probe_buffer;
  to.layout.offset = probes.size() * kProbeStride;
  to.layout.bytesPerRow = kProbeStride;
  wgpu::Extent3D size{1, 1, 1};
  encoder.CopyTextureToBuffer(&from, &to, &size);
  probes.push_back({pixel_shader, color->descriptor.handle, color->format});
}

bool Renderer::State::ClearSurface(const std::shared_ptr<SurfaceResource>& surface,
                                   uint32_t aspects, const ResolveRectangle& rectangle,
                                   const std::array<float, 4>& color, float depth,
                                   uint32_t stencil, std::string& error) {
  if (!surface) return true;
  const int32_t left = std::clamp(rectangle.left, 0, int32_t(surface->width));
  const int32_t top = std::clamp(rectangle.top, 0, int32_t(surface->height));
  const int32_t right = std::clamp(rectangle.right, left, int32_t(surface->width));
  const int32_t bottom = std::clamp(rectangle.bottom, top, int32_t(surface->height));
  if (right <= left || bottom <= top) return true;
  const bool full = !left && !top && uint32_t(right) == surface->width &&
                    uint32_t(bottom) == surface->height;
  EndPass();
  if (!Begin(error)) return false;
  if (full) {
    wgpu::RenderPassColorAttachment color_attachment{};
    wgpu::RenderPassDepthStencilAttachment depth_attachment{};
    wgpu::RenderPassDescriptor descriptor{};
    if (surface->depth) {
      const bool clear_depth = aspects & 2u, clear_stencil = aspects & 4u;
      depth_attachment.view = surface->view;
      depth_attachment.depthLoadOp =
          clear_depth || !surface->initialized ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
      depth_attachment.depthClearValue = clear_depth ? std::clamp(depth, 0.0f, 1.0f) : 0.0f;
      depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
      depth_attachment.stencilLoadOp =
          clear_stencil || !surface->initialized ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
      depth_attachment.stencilClearValue = clear_stencil ? (stencil & 0xFF) : 0;
      depth_attachment.stencilStoreOp = wgpu::StoreOp::Store;
      descriptor.depthStencilAttachment = &depth_attachment;
    } else {
      color_attachment.view = surface->view;
      color_attachment.loadOp = wgpu::LoadOp::Clear;
      color_attachment.storeOp = wgpu::StoreOp::Store;
      color_attachment.clearValue = {color[0], color[1], color[2], color[3]};
      descriptor.colorAttachmentCount = 1;
      descriptor.colorAttachments = &color_attachment;
    }
    encoder.BeginRenderPass(&descriptor).End();
  } else if (surface->depth) {
    if (aspects & 2u) {
      const std::array<float, 12> parameters{0, 0, 0, 0, 1, 1, 1, 1, depth, 0, 0, 0};
      if (!UtilityPass("clear_depth", surface->view, wgpu::kDepthSliceUndefined, true,
                       surface->format, surface->width, surface->height,
                       {left, top, right, bottom}, nullptr, parameters, surface->initialized,
                       error))
        return false;
    }
  } else {
    const std::array<float, 12> parameters{0, 0, 0, 0, 1, 1, 1, 1,
                                           color[0], color[1], color[2], color[3]};
    if (!UtilityPass("clear_color", surface->view, wgpu::kDepthSliceUndefined, false,
                     surface->format, surface->width, surface->height, {left, top, right, bottom},
                     nullptr, parameters, surface->initialized, error))
      return false;
  }
  surface->initialized = true;
  surface->content_serial = ++content_serial;
  return true;
}

bool Renderer::State::Clear(const Work& work, std::string& error) {
  const auto clear = work.As<ClearCommand>();
  ++stats.clears;
  std::array<float, 4> color{};
  for (size_t i = 0; i < color.size(); ++i) color[i] = std::bit_cast<float>(clear.color_bits[i]);
  const float depth = float(std::bit_cast<double>(clear.depth_bits));
  const ResolveRectangle rectangle{clear.left, clear.top, clear.right, clear.bottom};
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    if (!(clear.flags & (1u << i)) || !work.colors[i].handle) continue;
    auto surface = Surface(work.colors[i], false, error);
    if (!surface || !ClearSurface(surface, 1u, rectangle, color, depth, clear.stencil, error))
      return false;
  }
  const uint32_t aspects = ((clear.flags & 0x10u) ? 2u : 0u) | ((clear.flags & 0x20u) ? 4u : 0u);
  if (aspects && work.depth.handle) {
    auto surface = Surface(work.depth, true, error);
    if (!surface || !ClearSurface(surface, aspects, rectangle, color, depth, clear.stencil, error))
      return false;
  }
  return true;
}

}  // namespace rex::graphics::gta4_webgpu
