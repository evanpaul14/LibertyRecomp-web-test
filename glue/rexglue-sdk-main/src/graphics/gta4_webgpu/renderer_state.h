#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <emscripten/emscripten.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <webgpu/webgpu_cpp.h>
#include <xxhash.h>

#include <rex/graphics/gta4_native/environmental_data.h>
#include <rex/graphics/gta4_native/surface_view.h>
#include <rex/graphics/gta4_native/title_commands.h>
#include <rex/graphics/pipeline/texture/info.h>

#include "../gta4_native/core/draw_state.h"
#include "renderer.h"
#include "shader_archive.h"

REXCVAR_DECLARE(bool, webgpu_perf_report);

namespace rex::graphics::gta4_webgpu {

using Words = std::vector<uint32_t>;
struct WordsHash {
  size_t operator()(const Words& words) const {
    return size_t(XXH3_64bits(words.data(), words.size() * sizeof(uint32_t)));
  }
};

// A render target the title binds by surface handle.
struct SurfaceResource {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  gta4_native::SurfaceDescriptor descriptor{};
  bool depth = false;
  uint32_t width = 0, height = 0;
  bool initialized = false;
  uint64_t content_serial = 0;
};

// A sampled texture: uploaded from a guest capture, or produced by resolves.
struct TextureResource {
  wgpu::Texture texture;
  wgpu::TextureView view;  // Sampling view (2D, 2D array, cube or 3D).
  wgpu::TextureView first_layer;  // 2D view of layer 0 of an array, made on first use.
  wgpu::TextureViewDimension dimension = wgpu::TextureViewDimension::e2D;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  TextureInfo info{};
  xenos::xe_gpu_texture_fetch_t fetch{};
  uint64_t capture_generation = 0;
  bool gpu_produced = false;
  bool depth_values = false;  // r32float storage of a resolved depth surface.
  uint32_t width = 0, height = 0, layers = 1, mip_levels = 1;
  uint64_t content_serial = 0;
};

// Per-batch CPU staging mirrored into one GPU buffer right before submit.
struct Arena {
  wgpu::Buffer buffer;
  uint64_t capacity = 0;
  std::vector<uint8_t> bytes;
};

struct Pipeline {
  wgpu::RenderPipeline pipeline;
  wgpu::BindGroupLayout textures;  // Group 1; null when the shaders sample nothing.
  uint32_t texture_mask = 0, cube_mask = 0, sampler_mask = 0;
};

struct Targets {
  std::array<std::shared_ptr<SurfaceResource>, gta4_native::kRenderTargetCount> colors{};
  std::shared_ptr<SurfaceResource> depth;
  uint32_t width = 0, height = 0;
  bool operator==(const Targets& other) const {
    return colors == other.colors && depth == other.depth;
  }
  bool empty() const {
    for (const auto& color : colors) if (color) return false;
    return !depth;
  }
};

struct VertexBinding {
  wgpu::Buffer buffer;
  uint64_t offset = 0;
  uint64_t size = 0;
};

struct Renderer::State {
  State(memory::Memory* memory, const ShaderArchive* archive)
      : memory(memory), archive(archive) {}

  // --- renderer.cpp: device, batches, pipelines, utility passes ---
  void InitializeDevice();
  bool Begin(std::string& error);
  void EndPass();
  bool Flush(std::string& error);
  bool BeginPass(const Targets& targets, std::string& error);
  uint64_t PushUniforms(std::span<const uint8_t> bytes, std::string& error);
  uint64_t PushGeometry(std::span<const uint8_t> bytes, std::string& error);
  wgpu::ShaderModule Module(const ShaderRecord& record, bool late, std::string& error);
  wgpu::ShaderModule UtilityModule(const char* name, const char* code);
  wgpu::BindGroupLayout TextureLayout(uint32_t texture_mask, uint32_t cube_mask,
                                      uint32_t sampler_mask);
  wgpu::RenderPipeline UtilityPipeline(const std::string& name, wgpu::TextureFormat color,
                                       wgpu::TextureFormat depth, std::string& error);
  bool UtilityPass(const std::string& name, wgpu::TextureView target, uint32_t depth_slice,
                   bool target_is_depth, wgpu::TextureFormat format, uint32_t target_width,
                   uint32_t target_height, const std::array<int32_t, 4>& scissor,
                   wgpu::TextureView source, std::span<const float> parameters,
                   bool load_existing, std::string& error);

  // --- resources.cpp ---
  std::shared_ptr<SurfaceResource> Surface(const gta4_native::SurfaceDescriptor& descriptor,
                                           bool depth, std::string& error);
  std::shared_ptr<SurfaceResource> ResolveSource(const gta4_native::SurfaceDescriptor& descriptor,
                                                 bool depth, std::string& error);
  std::shared_ptr<TextureResource> CreateTexture(const xenos::xe_gpu_texture_fetch_t& fetch,
                                                 bool gpu_produced, std::string& error);
  std::shared_ptr<TextureResource> SampledTexture(uint32_t handle,
                                                  const xenos::xe_gpu_texture_fetch_t& fetch,
                                                  const std::shared_ptr<const TextureCapture>& capture,
                                                  std::string& error);
  bool Upload(TextureResource& texture, const TextureCapture& capture, std::string& error);
  wgpu::Sampler Sampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                        const TextureResource* texture);
  wgpu::Buffer IndexBuffer(const BufferCapture& capture, bool& index32, std::string& error);
  wgpu::Buffer VertexBuffer(const BufferCapture& capture, uint32_t offset, uint32_t stride,
                            std::span<const gta4_native::VertexElement* const> elements,
                            uint64_t& size, std::string& error);
  void ReleaseResource(uint32_t handle);
  void ClearResources();
  void BeginFrame();

  // --- draw.cpp ---
  bool Draw(const Work& work, std::string& error);
  bool Clear(const Work& work, std::string& error);
  bool ClearSurface(const std::shared_ptr<SurfaceResource>& surface, uint32_t aspects,
                    const gta4_native::ResolveRectangle& rectangle,
                    const std::array<float, 4>& color, float depth, uint32_t stencil,
                    std::string& error);
  Pipeline* DrawPipeline(const Targets& targets, const gta4_native::core::FixedFunctionState& fixed,
                         const ShaderRecord& vertex, const ShaderRecord* pixel, bool late,
                         uint32_t specialization, uint32_t topology, uint32_t strip_format,
                         const std::vector<wgpu::VertexBufferLayout>& layouts,
                         const std::vector<std::vector<wgpu::VertexAttribute>>& attributes,
                         uint32_t requested_colors, std::string& error);

  // --- passes.cpp ---
  bool Resolve(const Work& work, std::string& error);
  bool Handoff(const Work& work, std::string& error);
  Status Present(const Work& work, std::string& error);
  void PresentToCanvas(const TextureResource& source, std::string& error);
  Status Readback(const Work& work, std::string& error);

  memory::Memory* memory;
  const ShaderArchive* archive;
  std::function<void()> resume;

  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Limits limits{};
  wgpu::TextureFormat depth_format = wgpu::TextureFormat::Depth24PlusStencil8;
  bool float32_blendable = false;
  bool bc_textures = false;

  wgpu::Surface surface;  // The page canvas; absent under Node.
  wgpu::TextureFormat surface_format = wgpu::TextureFormat::Undefined;
  uint32_t surface_width = 0, surface_height = 0;

  wgpu::CommandEncoder encoder;
  wgpu::RenderPassEncoder pass;
  Targets pass_targets;
  // What the current pass already has bound (raw handles are only compared,
  // and the objects outlive the pass).
  struct PassState {
    WGPURenderPipeline pipeline = nullptr;
    bool uniforms_set = false;
    uint32_t uniform_offset = 0;
    WGPUBindGroup textures = nullptr;
    struct VertexSlot {
      WGPUBuffer buffer = nullptr;
      uint64_t offset = 0, size = 0;
    };
    std::array<VertexSlot, 16> vertex_buffers{};
    bool viewport_set = false, scissor_set = false, stencil_set = false, blend_set = false;
    std::array<float, 6> viewport{};
    std::array<uint32_t, 4> scissor{};
    uint32_t stencil_reference = 0;
    std::array<float, 4> blend{};
    WGPUBuffer index_buffer = nullptr;
    wgpu::IndexFormat index_format = wgpu::IndexFormat::Undefined;
  } pass_state;
  Arena uniforms, geometry;
  // Uniform slots already in this batch, by content hash (draws often repeat
  // the same constants).
  struct UniformSlot {
    uint64_t offset = 0;
    size_t size = 0;
  };
  std::unordered_map<uint64_t, UniformSlot> uniform_slots;
  wgpu::BindGroupLayout uniform_layout;
  wgpu::BindGroup uniform_group;  // Recreated when the uniform arena grows.
  wgpu::Buffer zero_vertices;
  wgpu::Texture fallback_2d, fallback_cube;
  wgpu::TextureView fallback_2d_view, fallback_cube_view;
  wgpu::Sampler fallback_sampler, linear_sampler;

  std::unordered_map<uint64_t, wgpu::ShaderModule> modules;
  std::unordered_map<std::string, wgpu::ShaderModule> utility_modules;
  std::unordered_map<uint64_t, wgpu::BindGroupLayout> texture_layouts;
  std::unordered_map<uint64_t, wgpu::PipelineLayout> pipeline_layouts;
  std::unordered_map<Words, Pipeline, WordsHash> pipelines;
  std::unordered_map<std::string, wgpu::RenderPipeline> utility_pipelines;
  std::unordered_map<Words, wgpu::Sampler, WordsHash> samplers;
  // Samplers by raw fetch constant and texture mip count, ahead of decoding
  // the fetch into `samplers`' key.
  using FetchKey = std::array<uint32_t, 7>;
  struct FetchKeyHash {
    size_t operator()(const FetchKey& key) const {
      return size_t(XXH3_64bits(key.data(), sizeof(key)));
    }
  };
  std::unordered_map<FetchKey, wgpu::Sampler, FetchKeyHash> fetch_samplers;

  // Title state carried by commands.
  std::unordered_map<uint32_t, const ShaderRecord*> shaders;
  std::unordered_map<uint32_t, std::vector<gta4_native::VertexElement>> declarations;
  struct Stream {
    uint32_t offset = 0, stride = 0;
  };
  std::array<Stream, gta4_native::kVertexStreamCount> streams{};
  uint32_t vertex_shader = 0, pixel_shader = 0, declaration = 0;
  gta4_native::EnvironmentalDataV2 environment{};

  // Resources by guest handle.
  std::unordered_map<uint32_t, std::shared_ptr<SurfaceResource>> surfaces;
  std::unordered_map<gta4_native::GuestPlacementKey, std::vector<uint32_t>,
                     gta4_native::GuestPlacementKeyHash>
      color_views;
  std::unordered_map<uint32_t, std::shared_ptr<TextureResource>> textures;
  struct ConvertedBuffer {
    wgpu::Buffer buffer;
    uint64_t size = 0;
    uint64_t frame = 0;
  };
  std::unordered_map<Words, ConvertedBuffer, WordsHash> vertex_buffers;
  std::unordered_map<uint64_t, ConvertedBuffer> index_buffers;
  std::unordered_map<Words, std::pair<wgpu::BindGroup, uint64_t>, WordsHash> texture_groups;

  uint64_t frame = 0;
  uint32_t submitted_frame = 0;  // The title's number of the last presented frame.
  uint64_t content_serial = 0;
  uint64_t draws = 0, frame_draws = 0, skipped_draws = 0;
  // Per-frame diagnostics, reported with presents.
  struct FrameStats {
    uint32_t draws = 0, no_targets = 0, empty_viewport = 0, empty_scissor = 0, failed = 0;
    uint32_t clears = 0, resolves = 0;
    std::array<uint32_t, 32> commands{};
  } stats;
  // Render-thread CPU time (ms) and work done since the last timing report.
  struct Timing {
    double execute_ms = 0, pipeline_ms = 0, texture_ms = 0, geometry_ms = 0, submit_ms = 0;
    double encode_ms = 0, uniform_ms = 0, inputs_ms = 0, bind_ms = 0;
    uint32_t frames = 0, draws = 0, new_pipelines = 0, new_textures = 0, new_buffers = 0;
    uint32_t new_groups = 0, uniform_slots = 0, uniform_reused = 0;
    uint64_t texture_bytes = 0, buffer_bytes = 0;
    double start_ms = 0;
  } timing;
  uint64_t gpu_errors = 0;
  bool ready = false;
  bool trace = false;
  uint64_t texture_failures = 0;
};

// Performance reports go to the info log, or to the warning log with
// --webgpu_perf_report so they show without the diagnostic flood.
#define WEBGPU_PERF_LOG(...)                                \
  do {                                                      \
    if (REXCVAR_GET(webgpu_perf_report))                    \
      REXLOG_WARN(__VA_ARGS__);                             \
    else                                                    \
      REXLOG_INFO(__VA_ARGS__);                             \
  } while (0)

// Adds the scope's wall time (ms) to `total`.
class ScopedTimer {
 public:
  explicit ScopedTimer(double& total) : total_(total), start_(emscripten_get_now()) {}
  ~ScopedTimer() { total_ += emscripten_get_now() - start_; }
  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;

 private:
  double& total_;
  double start_;
};

inline uint32_t GuestWord(const uint8_t* bytes, size_t offset) {
  uint32_t value;
  std::memcpy(&value, bytes + offset, sizeof(value));
  return __builtin_bswap32(value);
}

wgpu::TextureFormat SampledFormat(xenos::TextureFormat format);
wgpu::TextureFormat SurfaceFormat(uint32_t format, bool depth, wgpu::TextureFormat depth_format);
bool IsCompressed(wgpu::TextureFormat format);

}  // namespace rex::graphics::gta4_webgpu
