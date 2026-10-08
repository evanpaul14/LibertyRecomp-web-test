#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
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
  bool depth_values = false;  // rg32float (depth, stencil) of a resolved depth surface.
  // A packed depth alias: the source snapshot it was last built from.
  uint64_t packed_source_serial = 0;
  uint32_t packed_swizzle = 0;
  uint32_t width = 0, height = 0, layers = 1, mip_levels = 1;
  uint64_t content_serial = 0;
};

// Per-batch CPU staging mirrored into one GPU buffer right before submit.
struct Arena {
  wgpu::Buffer buffer;
  uint64_t capacity = 0;
  uint64_t used = 0;
  std::vector<uint8_t> bytes;  // `capacity` bytes; only the first `used` are this batch's.
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

// Converted vertex and index data, suballocated from large shared buffers so
// that draws from one buffer keep its binding and select their data with
// firstIndex/baseVertex: every pass call crosses into the browser's WebGPU.
class BufferPool {
 public:
  struct Range {
    uint32_t page = UINT32_MAX;  // UINT32_MAX: allocation failed.
    uint64_t offset = 0, size = 0;
  };
  void Initialize(wgpu::Device device, wgpu::BufferUsage usage, uint64_t page_size) {
    device_ = std::move(device);
    usage_ = usage;
    page_size_ = page_size;
  }
  // `alignment` need not be a power of two (vertex data aligns to its stride).
  Range Allocate(uint64_t size, uint64_t alignment);
  void Free(const Range& range);
  void Clear() { pages_.clear(); }
  const wgpu::Buffer& buffer(uint32_t page) const { return pages_[page].buffer; }
  uint64_t page_size(uint32_t page) const { return pages_[page].size; }

 private:
  struct Page {
    wgpu::Buffer buffer;  // Null once an oversized page is released.
    uint64_t size = 0;
    std::map<uint64_t, uint64_t> free;  // Offset -> size.
  };
  Range AllocateIn(uint32_t page, uint64_t size, uint64_t alignment);
  wgpu::Device device_;
  wgpu::BufferUsage usage_ = wgpu::BufferUsage::None;
  uint64_t page_size_ = 0;
  std::vector<Page> pages_;
};

// How a vertex shader's inputs are fed for one declaration and set of bound
// streams: a buffer per guest stream with every attribute decoded to float4,
// then a zero buffer for attributes the declaration lacks.
struct InputLayout {
  struct Stream {
    uint32_t stream = 0;
    std::vector<uint32_t> locations;
    std::vector<const gta4_native::VertexElement*> elements;  // Into `declarations`.
  };
  std::vector<Stream> streams;
  bool defaults = false;
  std::vector<wgpu::VertexBufferLayout> layouts;  // Attribute pointers unset.
  std::vector<std::vector<wgpu::VertexAttribute>> attributes;
  Words key;  // Its part of a pipeline key.
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
  // A new uniform slot of at least `size` bytes (256-aligned) to fill in
  // place; null on failure.
  uint8_t* ReserveUniforms(uint64_t size, uint64_t& offset, std::string& error);
  // Makes room for `size` more bytes of uniforms, flushing the batch (and so
  // forgetting its slots) when there is none.
  bool UniformSpace(uint64_t size, std::string& error);
  void ApplyDeviceDelta(const DeviceDelta& delta);
  // `alignment` (a multiple of 16) need not be a power of two.
  uint64_t PushGeometry(std::span<const uint8_t> bytes, std::string& error,
                        uint64_t alignment = 16);
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
                   bool load_existing, std::string& error,
                   wgpu::TextureView stencil = nullptr);

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
  std::shared_ptr<TextureResource> PackedDepthAlias(uint32_t handle, uint32_t source,
                                                    const xenos::xe_gpu_texture_fetch_t& fetch,
                                                    std::string& error);
  wgpu::Sampler Sampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                        const TextureResource* texture);
  // The converted indices' buffer, and the offset of index 0 in it.
  wgpu::Buffer IndexBuffer(const BufferCapture& capture, bool& index32, uint64_t& base,
                           std::string& error);
  // A binding of the converted stream; its offset is a multiple of the
  // converted stride (16 bytes per element).
  bool VertexBuffer(const BufferCapture& capture, uint32_t offset, uint32_t stride,
                    std::span<const gta4_native::VertexElement* const> elements,
                    VertexBinding& binding, std::string& error);
  void ReleaseResource(uint32_t handle);
  void ClearResources();
  void BeginFrame();

  // --- draw.cpp ---
  // False on error, or when a flush interrupted the draw's setup (`error`
  // stays empty then, and the draw can be retried in the new batch).
  bool Draw(const Work& work, std::string& error, bool retry = false);
  bool Clear(const Work& work, std::string& error);
  bool ClearSurface(const std::shared_ptr<SurfaceResource>& surface, uint32_t aspects,
                    const gta4_native::ResolveRectangle& rectangle,
                    const std::array<float, 4>& color, float depth, uint32_t stencil,
                    std::string& error);
  Pipeline* DrawPipeline(const Targets& targets, const gta4_native::core::FixedFunctionState& fixed,
                         const ShaderRecord& vertex, const ShaderRecord* pixel, bool late,
                         uint32_t specialization, uint32_t topology, uint32_t strip_format,
                         const InputLayout& inputs, uint32_t requested_colors, std::string& error);
  const InputLayout* Inputs(const ShaderRecord& vertex, uint32_t declaration,
                            const std::vector<gta4_native::VertexElement>& elements, bool up,
                            uint32_t bound_streams, std::string& error);

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
    std::array<uint32_t, kUniformBindings> uniform_offsets{};
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
  // The uniform slots most recently written in this batch (UINT64_MAX:
  // none). A draw binds them again while their inputs are unchanged: the
  // device's constants (by serial) or the shared constants (by value).
  struct LastUniforms {
    uint32_t device = 0;
    uint64_t vertex = UINT64_MAX, pixel = UINT64_MAX, shared = UINT64_MAX;
    uint64_t vertex_serial = 0, pixel_serial = 0;
    uint32_t specialization = 0;
    gta4_native::core::SharedConstants constants{};
  } last_uniforms;
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
  // Input layouts by vertex shader record, declaration handle and bound
  // streams (bit 31: a DrawPrimitiveUp draw). Cleared when declarations change.
  using InputKey = std::array<uint32_t, 4>;
  struct InputKeyHash {
    size_t operator()(const InputKey& key) const {
      return size_t(XXH3_64bits(key.data(), sizeof(key)));
    }
  };
  std::unordered_map<InputKey, InputLayout, InputKeyHash> input_layouts;
  // Per-draw scratch, kept to reuse its storage.
  Words pipeline_key, buffer_key, group_key;
  std::vector<wgpu::BindGroupEntry> group_entries;
  std::vector<VertexBinding> vertex_bindings;

  // Title state carried by commands.
  // Guest device blocks by address, kept current from each draw's delta.
  std::unordered_map<uint32_t, std::vector<uint8_t>> devices;
  // Bumped when a delta changes the vertex or pixel constants.
  uint64_t vertex_constants_serial = 0, pixel_constants_serial = 0;
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
  // Textures the title samples as A8R8G8B8 views of a resolved depth texture,
  // by alias handle: the resolved texture's handle.
  std::unordered_map<uint32_t, uint32_t> packed_depth_aliases;
  // Converted copies of guest buffers, in the pools below.
  struct ConvertedBuffer {
    BufferPool::Range range;
    uint64_t frame = 0;
    bool index32 = false;  // Index buffers: which pool.
  };
  std::unordered_map<Words, ConvertedBuffer, WordsHash> vertex_buffers;
  std::unordered_map<uint64_t, ConvertedBuffer> index_buffers;
  BufferPool vertex_pool, index16_pool, index32_pool;
  std::vector<uint8_t> convert_scratch;
  std::unordered_map<Words, std::pair<wgpu::BindGroup, uint64_t>, WordsHash> texture_groups;

  uint64_t frame = 0;
  uint64_t batches = 0;  // Flushes so far.
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
    double encode_ms = 0, uniform_ms = 0, inputs_ms = 0, bind_ms = 0, draw_ms = 0;
    uint32_t frames = 0, draws = 0, new_pipelines = 0, new_textures = 0, new_buffers = 0;
    uint32_t new_groups = 0;
    // New uniform slots written: vertex constants, pixel constants, shared.
    uint32_t vertex_slots = 0, pixel_slots = 0, shared_slots = 0;
    uint32_t redrawn = 0;  // Draws set up again after a flush interrupted them.
    // Render pass calls: pipelines, bind groups, vertex and index buffers,
    // fixed state (viewport, scissor, stencil reference, blend constant).
    uint32_t set_pipeline = 0, set_group = 0, set_vertex = 0, set_index = 0, set_state = 0;
    uint64_t texture_bytes = 0, buffer_bytes = 0;
    // Presents: shown on the canvas, skipped (no source, no canvas, no
    // surface texture), and how many showed a frontbuffer with new contents.
    uint32_t shown = 0, no_source = 0, no_canvas = 0, no_surface_texture = 0, new_content = 0;
    uint32_t surface_status = 0;  // wgpu::SurfaceGetCurrentTextureStatus of the last failure.
    double start_ms = 0;
  } timing;
  // The frontbuffer last shown, to tell a frozen picture from a frozen source.
  const TextureResource* presented_source = nullptr;
  uint64_t presented_serial = 0;
  uint32_t presented_frontbuffer = 0;
  uint64_t gpu_errors = 0;
  bool ready = false;
  bool trace = false;
  // --webgpu_trace_pixel: one texel of each traced draw's first color target,
  // copied after the draw and logged at present where it changed.
  struct PixelProbe {
    uint64_t pixel_shader = 0;
    uint32_t target = 0;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  };
  wgpu::Buffer probe_buffer;
  std::vector<PixelProbe> probes;
  void ProbePixel(const Targets& targets, uint64_t pixel_shader);
  void ReportPixelProbes(std::string& error);
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
