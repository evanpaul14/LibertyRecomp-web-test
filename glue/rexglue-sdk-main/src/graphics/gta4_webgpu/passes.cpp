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


uint32_t TexelBytes(wgpu::TextureFormat format) {
  switch (format) {
    case wgpu::TextureFormat::R8Unorm: return 1;
    case wgpu::TextureFormat::RG8Unorm: return 2;
    case wgpu::TextureFormat::RGBA8Unorm:
    case wgpu::TextureFormat::RG16Float:
    case wgpu::TextureFormat::R32Float: return 4;
    case wgpu::TextureFormat::RGBA16Float:
    case wgpu::TextureFormat::RG32Float: return 8;
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

wgpu::TextureView StencilView(const SurfaceResource& surface) {
  wgpu::TextureViewDescriptor view{};
  view.aspect = wgpu::TextureAspect::StencilOnly;
  return surface.texture.CreateView(&view);
}
}  // namespace

void Renderer::State::PresentToCanvas(const TextureResource& source, std::string& error) {
  uint32_t width = 0, height = 0;
  if (!PollCanvas(width, height)) {
    ++timing.no_canvas;
    return;
  }
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
  if (!current.texture) {
    ++timing.no_surface_texture;
    timing.surface_status = uint32_t(current.status);
    return;
  }
  ++timing.shown;
  if (&source != presented_source || source.content_serial != presented_serial)
    ++timing.new_content;
  presented_source = &source;
  presented_serial = source.content_serial;
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
  // The surface holding the contents may view the same EDRAM with another MSAA
  // layout (the title downsamples by resolving a 4x view of a 1x surface).
  // The rectangle is then in the requested view's pixels.
  const bool reinterpret = !depth && c.source.sample_type != source->descriptor.sample_type;
  const int32_t logical_w = int32_t(reinterpret ? c.source.width : source->width);
  const int32_t logical_h = int32_t(reinterpret ? c.source.height : source->height);
  auto rect = c.source_rectangle_valid ? c.source_rectangle
                                       : ResolveRectangle{0, 0, logical_w, logical_h};
  rect.left = std::clamp(rect.left, 0, logical_w);
  rect.top = std::clamp(rect.top, 0, logical_h);
  rect.right = std::clamp(rect.right, rect.left, logical_w);
  rect.bottom = std::clamp(rect.bottom, rect.top, logical_h);
  const uint32_t copy_w = std::min(uint32_t(rect.right - rect.left), target_w - uint32_t(dx));
  const uint32_t copy_h = std::min(uint32_t(rect.bottom - rect.top), target_h - uint32_t(dy));
  if (trace)
    REXLOG_INFO("webgpu-trace: resolve flags={:08X} source={:08X} chosen={:08X} serial={} "
                "destination={:08X} {}x{} rect={},{},{},{} copy={}x{} at {},{} samples={}/{}",
                flags, c.source.handle, source->descriptor.handle, source->content_serial,
                c.destination_texture, destination->width, destination->height, rect.left,
                rect.top, rect.right, rect.bottom, copy_w, copy_h, dx, dy, c.source.sample_type,
                source->descriptor.sample_type);
  EndPass();
  if (!Begin(error)) return false;
  if (copy_w && copy_h) {
    const int32_t exponent = depth ? 0 : NativeResolveExponent(flags);
    if (reinterpret) {
      uint32_t depth_slice = 0;
      auto target = Subresource(*destination, level, slice, depth_slice);
      const auto requested = xenos::MsaaSamples(c.source.sample_type);
      const auto owner = xenos::MsaaSamples(source->descriptor.sample_type);
      const auto sample_select =
          SanitizeGuestCopySampleSelect(DecodeResolveSampleSelect(flags), requested, false);
      const std::array<float, 12> parameters{
          float(rect.left), float(rect.top), float(dx), float(dy),
          float(GuestSampleScaleX(requested)), float(GuestSampleScaleY(requested)),
          float(GuestSampleScaleX(owner)), float(GuestSampleScaleY(owner)),
          std::ldexp(1.0f, exponent), float(uint32_t(sample_select)), 0, 0};
      if (!UtilityPass("resolve_color", target, depth_slice, false, destination->format, target_w,
                       target_h, {dx, dy, dx + int32_t(copy_w), dy + int32_t(copy_h)},
                       source->view, parameters, true, error))
        return false;
    } else if (!depth && !exponent && source->format == destination->format) {
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
                       depth ? DepthView(*source) : source->view, parameters, true, error,
                       depth ? StencilView(*source) : nullptr))
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
  if (!IsValidForwardStencilHandoffPolicy(c.stencil_policy)) {
    error = "Invalid depth handoff stencil policy";
    return false;
  }
  const bool rebuild = c.stencil_policy == ForwardStencilHandoffPolicy::kRebuildSceneCoverage;
  auto destination = Surface(c.destination, true, error);
  if (!destination) return false;
  // As in the Metal renderer, depth comes from the title's resolved snapshot.
  std::shared_ptr<TextureResource> snapshot;
  if (auto found = textures.find(c.source_texture); found != textures.end() &&
                                                     found->second->gpu_produced &&
                                                     found->second->depth_values &&
                                                     found->second->content_serial)
    snapshot = found->second;
  if (trace)
    REXLOG_INFO("webgpu-trace: handoff source={:08X} texture={:08X} snapshot={} destination={:08X} "
                "rebuild={}",
                c.source.handle, c.source_texture, bool(snapshot), c.destination.handle, rebuild);
  const std::array<int32_t, 4> whole{0, 0, int32_t(destination->width),
                                     int32_t(destination->height)};
  if (snapshot) {
    if (snapshot->width != destination->width || snapshot->height != destination->height) {
      error = "Depth handoff between different extents";
      return false;
    }
    wgpu::TextureViewDescriptor level{};
    level.dimension = wgpu::TextureViewDimension::e2D;
    level.mipLevelCount = 1;
    level.arrayLayerCount = 1;
    const bool float_depth = GetBaseFormat(snapshot->info.format) == xenos::TextureFormat::k_24_8_FLOAT;
    const std::array<float, 12> parameters{0, 0, 0, 0, 0, 0, 0, 0, float_depth ? 1.0f : 0.0f, 0, 0, 0};
    // A rebuild clears depth to zero and stencil to the empty-scene value;
    // otherwise every depth sample is overwritten and the stencil is kept.
    if (!UtilityPass(rebuild ? "scene_depth_handoff" : "copy_depth_values", destination->view,
                     wgpu::kDepthSliceUndefined, true, destination->format, destination->width,
                     destination->height, whole, snapshot->texture.CreateView(&level), parameters,
                     !rebuild && destination->initialized, error))
      return false;
  } else {
    // No snapshot: copy the source surface's depth and keep the stencil.
    auto source = ResolveSource(c.source, true, error);
    if (!source) return false;
    if (!source->initialized || source == destination) return true;
    if (source->width != destination->width || source->height != destination->height) {
      error = "Depth handoff between different extents";
      return false;
    }
    const std::array<float, 12> parameters{0, 0, 0, 0,
                                           float(source->width), float(source->height),
                                           float(source->width), float(source->height),
                                           0, 0, 0, 0};
    if (!UtilityPass("copy_depth", destination->view, wgpu::kDepthSliceUndefined, true,
                     destination->format, destination->width, destination->height, whole,
                     DepthView(*source), parameters, destination->initialized, error))
      return false;
  }
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
    if (REXCVAR_GET(webgpu_early_frame_ack)) return;  // Done at capture.
    if (c.device && uint64_t(c.device) + kDeviceCompletedFrame + 4 <= 0x100000000ull) {
      const uint32_t completed = __builtin_bswap32(c.submitted_frame);
      std::memcpy(memory->TranslateVirtual<uint8_t*>(c.device + kDeviceCompletedFrame),
                  &completed, sizeof(completed));
    }
  };
  if (c.submitted_frame <= 3 || c.submitted_frame % 60 == 0)
    REXLOG_INFO("gta4-webgpu: frame={} draws={} no-targets={} empty-viewport={} empty-scissor={} "
                "failed={} waiting={} clears={} resolves={} frontbuffer={:08X} source={} {}x{} "
                "pipelines={} textures={} surfaces={}",
                c.submitted_frame, stats.draws, stats.no_targets, stats.empty_viewport,
                stats.empty_scissor, stats.failed, stats.waiting, stats.clears, stats.resolves,
                c.frontbuffer_texture, !source ? "none" : source->gpu_produced ? "gpu" : "cpu",
                source ? source->width : 0, source ? source->height : 0, pipelines.size(),
                textures.size(), surfaces.size());
  if (c.submitted_frame <= 3 || c.submitted_frame % 60 == 0) {
    std::string counts;
    for (size_t i = 0; i < stats.commands.size(); ++i)
      if (stats.commands[i]) counts += fmt::format(" {}:{}", i, stats.commands[i]);
    REXLOG_INFO("gta4-webgpu: frame={} commands{}", c.submitted_frame, counts);
  }
  ++timing.frames;
  timing.draws += stats.draws;
  const double now = emscripten_get_now();
  if (last_present_ms && now - last_present_ms > timing.longest_ms) {
    timing.longest_ms = now - last_present_ms;
    timing.longest_busy_ms = timing.execute_ms - last_execute_ms;
    timing.longest_pipeline_ms = timing.pipeline_ms - last_pipeline_ms;
  }
  if (!timing.start_ms) timing.start_ms = now;
  if (now - timing.start_ms >= 5000.0) {
    const double frames = timing.frames, elapsed = now - timing.start_ms;
    const double draws = std::max(1u, timing.draws);
    // Stage times are only measured with the flag.
    if (REXCVAR_GET(webgpu_perf_report)) {
      WEBGPU_PERF_LOG("gta4-webgpu: perf {:.1f} fps over {} frames; per frame: {:.0f} draws in "
                      "{:.1f} passes, render-thread {:.2f} ms (draws {:.2f}: pipelines {:.2f}, textures {:.2f}, "
                      "geometry {:.2f}, inputs {:.2f}, bindings {:.2f}, uniforms {:.2f}, encode "
                      "{:.2f}; submit {:.2f}); new pipelines={} (ready {}, failed {}, latency avg "
                      "{:.0f} max {:.0f} ms, {:.0f} draws a frame waited; warmed {} shader modules, {:.2f} ms a "
                      "frame, {} queued) textures={} ({} KB) "
                      "buffers={} ({} "
                      "KB) groups={}; new uniform slots: vertex {}+{} pixel {}+{} (hot+cold) shared {}; redrawn {}; calls per draw: "
                      "pipeline {:.2f}, group {:.2f}, vertex {:.2f}, index {:.2f}, state {:.2f}",
                      frames * 1000.0 / elapsed, timing.frames, timing.draws / frames,
                      timing.passes / frames,
                      timing.execute_ms / frames, timing.draw_ms / frames,
                      timing.pipeline_ms / frames, timing.texture_ms / frames,
                      timing.geometry_ms / frames, timing.inputs_ms / frames,
                      timing.bind_ms / frames, timing.uniform_ms / frames,
                      timing.encode_ms / frames, timing.submit_ms / frames, timing.new_pipelines,
                      timing.pipelines_ready, timing.pipelines_failed,
                      timing.pipelines_ready + timing.pipelines_failed
                          ? timing.pipeline_latency_ms /
                                (timing.pipelines_ready + timing.pipelines_failed)
                          : 0.0,
                      timing.pipeline_latency_max_ms, timing.waiting_draws / frames,
                      timing.warmed_modules, timing.warmup_ms / frames,
                      module_queue.size() + late_module_queue.size(),
                      timing.new_textures, timing.texture_bytes / 1024, timing.new_buffers,
                      timing.buffer_bytes / 1024, timing.new_groups, timing.part_slots[kVertexHot],
                      timing.part_slots[kVertexCold], timing.part_slots[kPixelHot],
                      timing.part_slots[kPixelCold], timing.shared_slots, timing.redrawn,
                      timing.set_pipeline / draws, timing.set_group / draws,
                      timing.set_vertex / draws, timing.set_index / draws, timing.set_state / draws);
      if (!recipe_queue.empty() || recipes_in_flight || timing.recipes_started ||
          timing.recipes_used)
        WEBGPU_PERF_LOG("gta4-webgpu: pipeline recipes started={} ready={} failed={} drawn={} "
                        "queued={} compiling={} known={}",
                        timing.recipes_started, timing.recipes_ready, timing.recipes_failed,
                        timing.recipes_used, recipe_queue.size(), recipes_in_flight,
                        recipes_known.size());
      WEBGPU_PERF_LOG("gta4-webgpu: presents shown={} new-content={} no-source={} no-canvas={} "
                      "no-surface-texture={} (status {}); canvas {}x{}; frontbuffer {:08X} "
                      "({}, {}x{}, serial {}); gpu errors {}",
                      timing.shown, timing.new_content, timing.no_source, timing.no_canvas,
                      timing.no_surface_texture, timing.surface_status, surface_width,
                      surface_height, c.frontbuffer_texture,
                      !source ? "none" : source->gpu_produced ? "gpu" : "cpu",
                      source ? source->width : 0, source ? source->height : 0,
                      source ? source->content_serial : 0, gpu_errors);
      WEBGPU_PERF_LOG("gta4-webgpu: longest frame {:.0f} ms (render thread busy {:.0f}, pipelines "
                      "{:.0f}); gpu frame latency avg {:.1f} max {:.0f} ms, {} presents waited "
                      "for the gpu",
                      timing.longest_ms, timing.longest_busy_ms, timing.longest_pipeline_ms,
                      timing.gpu_frames ? timing.gpu_latency_ms / timing.gpu_frames : 0.0,
                      timing.gpu_latency_max_ms, timing.gpu_waits);
      {
        std::string chunks;
        for (size_t i = 0; i < timing.constant_chunks.size(); ++i)
          chunks += fmt::format("{}{:.0f}", i == 0 ? "" : i == 32 ? " | " : " ",
                                timing.constant_chunks[i] / frames);
        WEBGPU_PERF_LOG("gta4-webgpu: constant chunk changes a frame (8 registers each; vertex | "
                        "pixel): {}", chunks);
      }
      if (timing.gpu_timed_frames) {
        const double timed = timing.gpu_timed_frames;
        std::vector<std::pair<std::string, Timing::GpuPass>> passes(timing.gpu_passes.begin(),
                                                                    timing.gpu_passes.end());
        std::sort(passes.begin(), passes.end(),
                  [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
        WEBGPU_PERF_LOG("gta4-webgpu: gpu passes {:.2f} ms a frame, idle between them {:.2f} "
                        "(first start to last end {:.2f} ms), idle between frames {:.2f} ms; "
                        "over {} timed frames, {} labels, {} frames not timed",
                        timing.gpu_pass_ms / timed, timing.gpu_idle_ms / timed,
                        timing.gpu_span_ms / timed, timing.gpu_frame_gap_ms / timed,
                        timing.gpu_timed_frames, passes.size(), timing.gpu_timer_drops);
        for (size_t i = 0; i < std::min<size_t>(passes.size(), 16); ++i) {
          const auto& [label, pass] = passes[i];
          WEBGPU_PERF_LOG("gta4-webgpu: gpu {:6.2f} ms a frame ({:4.1f}%), {:.1f} passes a frame, "
                          "{:.0f} draws a pass, max {:.2f} ms (ps {:016X}): {}",
                          pass.ms / timed, 100.0 * pass.ms / std::max(timing.gpu_pass_ms, 1e-9),
                          pass.passes / timed, pass.passes ? double(pass.draws) / pass.passes : 0.0,
                          pass.max_ms, pass.pixel_shader, label);
        }
      }
    }
    timing = {};
    timing.start_ms = now;
  }
  last_present_ms = now;
  last_execute_ms = timing.execute_ms;
  last_pipeline_ms = timing.pipeline_ms;
  stats = {};
  frame_draws = 0;
  submitted_frame = c.submitted_frame;

  if (trace && !probes.empty()) ReportPixelProbes(error);
  if (source)
    PresentToCanvas(*source, error);
  else
    ++timing.no_source;
  ResolvePassTimers(error);
  const std::string dump_path = REXCVAR_GET(webgpu_frame_dump_path);
  const uint32_t interval = std::max(1u, REXCVAR_GET(webgpu_frame_dump_interval));
  // The traced frame is always dumped, so its picture matches its trace.
  const bool dump = !dump_path.empty() && source && c.width && c.height &&
                    (c.submitted_frame % interval == 0 || trace);
  if (!dump) {
    Flush(error);
    acknowledge();
    BeginFrame();
    if (!source && error.empty()) error = "Title frontbuffer is not a ready color image";
    return TrackGpuFrame();
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

Renderer::Status Renderer::State::TrackGpuFrame() {
  // Nothing else stops this thread from running ahead of the GPU: frames
  // would queue up behind slow GPU work (such as shader compiles) and the
  // canvas would skip from the oldest to the newest.
  constexpr uint32_t kMaximumGpuFrames = 2;
  const double submitted = emscripten_get_now();
  ++gpu_frames_pending;
  queue.OnSubmittedWorkDone(
      wgpu::CallbackMode::AllowSpontaneous,
      [this, submitted](wgpu::QueueWorkDoneStatus, wgpu::StringView) {
        const double latency = emscripten_get_now() - submitted;
        timing.gpu_latency_ms += latency;
        timing.gpu_latency_max_ms = std::max(timing.gpu_latency_max_ms, latency);
        ++timing.gpu_frames;
        --gpu_frames_pending;
        if (gpu_waiting && gpu_frames_pending < kMaximumGpuFrames) {
          gpu_waiting = false;
          if (resume) resume();
        }
      });
  if (gpu_frames_pending < kMaximumGpuFrames) return Status::kYield;
  gpu_waiting = true;
  ++timing.gpu_waits;
  return Status::kPending;
}

const wgpu::PassTimestampWrites* Renderer::State::TimePass(std::string label) {
  if (!timestamp_queries) return nullptr;
  if (pass_timers.size() >= kMaximumPassTimers) return nullptr;
  const uint32_t index = uint32_t(pass_timers.size());
  pass_timers.push_back({std::move(label)});
  timestamp_writes = {};
  timestamp_writes.querySet = timestamp_queries;
  timestamp_writes.beginningOfPassWriteIndex = index * 2;
  timestamp_writes.endOfPassWriteIndex = index * 2 + 1;
  return &timestamp_writes;
}

std::string Renderer::State::TargetsLabel(const Targets& targets) const {
  const auto name = [](wgpu::TextureFormat format) -> std::string_view {
    switch (format) {
      case wgpu::TextureFormat::RGBA8Unorm: return "rgba8";
      case wgpu::TextureFormat::RGBA16Float: return "rgba16f";
      case wgpu::TextureFormat::R32Float: return "r32f";
      case wgpu::TextureFormat::RG16Float: return "rg16f";
      case wgpu::TextureFormat::Depth32FloatStencil8: return "d32s8";
      case wgpu::TextureFormat::Depth24PlusStencil8: return "d24s8";
      default: return "?";
    }
  };
  std::string label = fmt::format("title {}x{}", targets.width, targets.height);
  for (uint32_t i = 0; i < kRenderTargetCount; ++i)
    if (const auto& color = targets.colors[i])
      label += fmt::format(" c{}={:08X}/{}", i, color->descriptor.handle, name(color->format));
  if (targets.depth)
    label += fmt::format(" d={:08X}/{}", targets.depth->descriptor.handle, name(targets.depth->format));
  return label;
}

void Renderer::State::ResolvePassTimers(std::string& error) {
  if (pass_timers.empty()) return;
  int free_slot = -1;
  for (size_t i = 0; i < timer_readbacks.size(); ++i)
    if (!timer_readbacks[i].busy && int(i) != pending_timer_readback) free_slot = int(i);
  if (free_slot < 0 || pending_timer_readback >= 0) {
    ++timing.gpu_timer_drops;
    pass_timers.clear();
    return;
  }
  EndPass();
  if (!Begin(error)) return;
  auto& slot = timer_readbacks[free_slot];
  const uint32_t queries = uint32_t(pass_timers.size()) * 2;
  encoder.ResolveQuerySet(timestamp_queries, 0, queries, slot.resolve, 0);
  encoder.CopyBufferToBuffer(slot.resolve, 0, slot.readback, 0, queries * sizeof(uint64_t));
  slot.busy = true;
  pending_timer_readback = free_slot;
  pending_timer_passes = std::move(pass_timers);
  pass_timers.clear();
}

void Renderer::State::MapPassTimers() {
  auto& slot = timer_readbacks[pending_timer_readback];
  pending_timer_readback = -1;
  auto passes = std::move(pending_timer_passes);
  pending_timer_passes.clear();
  const uint64_t bytes = passes.size() * 2 * sizeof(uint64_t);
  slot.readback.MapAsync(
      wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::AllowSpontaneous,
      [this, &slot, passes = std::move(passes), bytes](wgpu::MapAsyncStatus status,
                                                       wgpu::StringView) {
        if (status == wgpu::MapAsyncStatus::Success) {
          const auto* stamps = static_cast<const uint64_t*>(slot.readback.GetConstMappedRange(0, bytes));
          // The GPU overlaps neighboring passes, so each pass is charged
          // only for how far it moved the end of the timeline; time with no
          // pass running is idle.
          uint64_t first = UINT64_MAX, cursor = 0;
          double total = 0, idle = 0;
          for (size_t i = 0; i < passes.size(); ++i) {
            const uint64_t begin = stamps[i * 2], end = stamps[i * 2 + 1];
            if (!begin || end < begin) continue;
            if (first == UINT64_MAX) {
              if (gpu_last_frame_end && begin > gpu_last_frame_end)
                timing.gpu_frame_gap_ms += double(begin - gpu_last_frame_end) / 1e6;
              first = cursor = begin;
            }
            if (begin > cursor) idle += double(begin - cursor) / 1e6;
            const double ms = end > std::max(begin, cursor)
                                  ? double(end - std::max(begin, cursor)) / 1e6 : 0.0;
            cursor = std::max(cursor, end);
            total += ms;
            auto& entry = timing.gpu_passes[passes[i].label];
            entry.ms += ms;
            ++entry.passes;
            entry.draws += passes[i].draws;
            if (ms > entry.max_ms) {
              entry.max_ms = ms;
              entry.pixel_shader = passes[i].pixel_shader;
            }
          }
          timing.gpu_pass_ms += total;
          timing.gpu_idle_ms += idle;
          if (first != UINT64_MAX) {
            timing.gpu_span_ms += double(cursor - first) / 1e6;
            gpu_last_frame_end = cursor;
          }
          ++timing.gpu_timed_frames;
          slot.readback.Unmap();
        }
        slot.busy = false;
      });
}

void Renderer::State::ReportPixelProbes(std::string& error) {
  Flush(error);
  auto buffer = std::move(probe_buffer);
  auto list = std::move(probes);
  probe_buffer = nullptr;
  probes.clear();
  const uint64_t bytes = list.size() * 256;
  buffer.MapAsync(
      wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::AllowSpontaneous,
      [buffer, list](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (status != wgpu::MapAsyncStatus::Success) return;
        const auto* data = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, list.size() * 256));
        std::unordered_map<uint32_t, std::string> last;
        for (size_t i = 0; i < list.size(); ++i) {
          const uint32_t size = std::max(1u, TexelBytes(list[i].format));
          std::string value;
          for (uint32_t b = 0; b < size; ++b) value += fmt::format("{:02X}", data[i * 256 + b]);
          auto& previous = last[list[i].target];
          if (value != previous)
            REXLOG_INFO("webgpu-pixel: draw#{} ps={:016X} target={:08X} {} -> {}", i,
                        list[i].pixel_shader, list[i].target, previous.empty() ? "?" : previous,
                        value);
          previous = value;
        }
        buffer.Unmap();
      });
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
                float value, stencil;
                std::memcpy(&value, texel, sizeof(value));
                std::memcpy(&stencil, texel + sizeof(value), sizeof(stencil));
                const uint32_t quantized =
                    float_depth ? xenos::Float32To20e4(value, false)
                                : uint32_t(std::nearbyint(std::clamp(double(value), 0.0, 1.0) *
                                                          16777215.0));
                packed = quantized << 8 | (uint32_t(stencil) & 0xFFu);
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
