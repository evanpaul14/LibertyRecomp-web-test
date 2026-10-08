#include "graphics_system.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <thread>

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <rex/graphics/gta4_native/fire_escape_trace.h>
#include <rex/graphics/gta4_native/gpu_pass_origin.h>
#include <rex/graphics/gta4_native/phone_trace.h>
#include <rex/graphics/gta4_native/temporal_commands.h>
#include <rex/graphics/gta4_native/tv_trace.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>
#include <rex/thread/wait_trace.h>

#include "../gta4_native/native_buffer_metadata.h"
#include "../gta4_native/native_texture_image_identity.h"
#include "canvas.h"
#include "renderer.h"
#include "renderer_state.h"
#include "shader_archive.h"
#include "simd_bytes.h"

REXCVAR_DEFINE_UINT32(webgpu_frame_limit, 60, "GPU",
                      "Web build: most frames presented per second (0 = unlimited)");

namespace rex::graphics::gta4_webgpu {
// The render thread's pending yield (see YieldToEventLoop).
void (*yield_resume)(void*) = nullptr;
void* yield_self = nullptr;

extern "C" EMSCRIPTEN_KEEPALIVE void liberty_gpu_resume_drain() {
  if (yield_resume) yield_resume(yield_self);
}

namespace {
// Posts a message to this worker so liberty_gpu_resume_drain runs as a new
// task, after the current one ends and the browser shows the canvas.
EM_JS(void, liberty_gpu_yield, (), {
  if (!globalThis.libertyGpuYield) {
    var channel = new MessageChannel();
    channel.port1.onmessage = () => _liberty_gpu_resume_drain();
    globalThis.libertyGpuYield = channel;
  }
  globalThis.libertyGpuYield.port2.postMessage(0);
});

using namespace gta4_native;

constexpr size_t kMaximumQueuedCommands = 65536;
constexpr uint32_t kMaximumQueuedPresents = 2;
constexpr uint32_t kMaximumBufferBytes = 64u * 1024u * 1024u;
constexpr size_t kMaximumTextureBytes = 128u * 1024u * 1024u;
constexpr size_t kMaximumUpBytes = 4u * 1024u * 1024u;

// Guest device block fields (see gta4_native/core/draw_state.h).
constexpr uint32_t kDeviceIndexBuffer = 12428;
constexpr uint32_t kDeviceColorTargets = 12432;
constexpr uint32_t kDeviceDepthTarget = 12448;
constexpr uint32_t kDeviceVertexStreams = 12452;
constexpr uint32_t kDeviceTextures = 0x30F8;
constexpr uint32_t kDeviceFetchConstants = 0x480;

uint32_t LoadBig32(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, bytes, sizeof(value));
  return __builtin_bswap32(value);
}

size_t TitleCommandSize(CommandType type) {
  switch (type) {
    case CommandType::kDeviceCreated:
    case CommandType::kDeviceDestroyed: return sizeof(DeviceCommand);
    case CommandType::kRegisterShader: return sizeof(RegisterShaderCommand);
    case CommandType::kRegisterVertexDeclaration: return sizeof(RegisterVertexDeclarationCommand);
    case CommandType::kSetRenderState: return sizeof(SetRenderStateCommand);
    case CommandType::kSetPixelShader:
    case CommandType::kSetVertexShader: return sizeof(SetShaderCommand);
    case CommandType::kSetVertexDeclaration: return sizeof(SetVertexDeclarationCommand);
    case CommandType::kSetTexture: return sizeof(SetTextureCommand);
    case CommandType::kResourceUnlock: return sizeof(ResourceUnlockCommand);
    case CommandType::kSetDepthStencil: return sizeof(SetDepthStencilCommand);
    case CommandType::kSetRenderTarget: return sizeof(SetRenderTargetCommand);
    case CommandType::kSetVertexStream: return sizeof(SetVertexStreamCommand);
    case CommandType::kSetIndexBuffer: return sizeof(SetIndexBufferCommand);
    case CommandType::kDrawPrimitive: return sizeof(DrawPrimitiveCommand);
    case CommandType::kDrawPrimitiveUp: return sizeof(DrawPrimitiveUpCommand);
    case CommandType::kDrawIndexedPrimitive: return sizeof(DrawIndexedPrimitiveCommand);
    case CommandType::kResolve: return sizeof(ResolveCommand);
    case CommandType::kTextureLock: return sizeof(TextureLockCommand);
    case CommandType::kClear: return sizeof(ClearCommand);
    case CommandType::kRenderPhaseMarker: return sizeof(RenderPhaseMarkerCommand);
    case CommandType::kPresent: return sizeof(PresentCommand);
    case CommandType::kQueryDeviceCapabilities: return sizeof(QueryDeviceCapabilitiesCommand);
    case CommandType::kRegisterReflectionTarget: return sizeof(RegisterReflectionTargetCommand);
    case CommandType::kReleaseResource: return sizeof(ReleaseResourceCommand);
    case CommandType::kUpdateEnvironmentalData: return sizeof(UpdateEnvironmentalDataCommand);
    case CommandType::kDepthSurfaceHandoff: return sizeof(DepthSurfaceHandoffCommand);
    case CommandType::kRegisterVirtualResource: return sizeof(RegisterVirtualResourceCommand);
    case CommandType::kTemporalUpdate: return sizeof(TemporalCommand);
    case CommandType::kQueryTemporalUpscaler: return 0;
  }
  return 0;
}

// Diagnostic envelopes are transport metadata around a title command.
bool Unwrap(uint32_t& abi, const void*& data, size_t& size) {
  if (!data || size < sizeof(CommandHeader)) return false;
  for (unsigned depth = 0; depth < 5; ++depth) {
    if (abi == kTitleCommandAbi) return true;
    if (abi == kGpuPassEnvelopeAbi) {
      GpuPassOrigin origin{};
      if (!UnpackGpuPassEnvelope(data, size, abi, origin)) return false;
    } else if (abi == kFireTraceEnvelopeAbi) {
      FireTraceContext context{};
      if (!UnpackFireTraceEnvelope(data, size, abi, context)) return false;
    } else if (abi == kTvTraceEnvelopeAbi) {
      TvTraceContext context{};
      if (!UnpackTvTraceEnvelope(data, size, abi, context)) return false;
    } else if (abi == kPhoneTraceEnvelopeAbi) {
      PhoneTraceContext context{};
      if (!UnpackPhoneTraceEnvelope(data, size, abi, context)) return false;
    } else {
      return false;
    }
  }
  return false;
}

template <class T>
T Read(const void* command) {
  T value{};
  std::memcpy(&value, command, sizeof(value));
  return value;
}
}  // namespace

Gta4WebGpuGraphicsSystem::Gta4WebGpuGraphicsSystem() = default;
Gta4WebGpuGraphicsSystem::~Gta4WebGpuGraphicsSystem() { Shutdown(); }

X_STATUS Gta4WebGpuGraphicsSystem::SetupPresentation(ui::WindowedAppContext*) {
  // The render thread presents to the page's canvas itself; there is no
  // ui::Presenter (and so no ImGui overlay) on this backend.
  presentation_ready_ = true;
  return X_STATUS_SUCCESS;
}

X_STATUS Gta4WebGpuGraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* dispatcher,
                                                 system::KernelState*) {
  if (render_thread_started_) return ready_ok_ ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
  if (!dispatcher || !dispatcher->memory()) return X_STATUS_INVALID_PARAMETER;
  memory_ = dispatcher->memory();
  std::string error;
  archive_ = GetShaderArchive(error);
  if (!archive_) {
    REXLOG_ERROR("gta4-webgpu: {}", error);
    return X_STATUS_UNSUCCESSFUL;
  }
  renderer_ = std::make_unique<Renderer>(memory_, archive_);
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 4u * 1024u * 1024u);
  const int created = pthread_create(&render_thread_, &attributes, RenderThreadMain, this);
  pthread_attr_destroy(&attributes);
  if (created) {
    REXLOG_ERROR("gta4-webgpu: render thread creation failed ({})", created);
    return X_STATUS_UNSUCCESSFUL;
  }
  render_thread_started_ = true;
  std::unique_lock lock(ready_mutex_);
  ready_wake_.wait(lock, [this] { return ready_done_; });
  if (!ready_ok_) {
    REXLOG_ERROR("gta4-webgpu: {}", ready_error_);
    return X_STATUS_UNSUCCESSFUL;
  }
  REXLOG_INFO("gta4-webgpu: title-command renderer connected; abi={} shaders={}",
              kTitleCommandAbi, archive_->size());
  return X_STATUS_SUCCESS;
}

void* Gta4WebGpuGraphicsSystem::RenderThreadMain(void* argument) {
  auto* self = static_cast<Gta4WebGpuGraphicsSystem*>(argument);
  RequestCanvas();
  self->renderer_->SetResume([self] {
    {
      std::lock_guard lock(self->queue_mutex_);
      self->waiting_on_gpu_ = false;
    }
    self->Drain();
  });
  self->renderer_->Initialize([self](bool ok, const std::string& error) {
    {
      std::lock_guard lock(self->ready_mutex_);
      self->ready_ok_ = ok;
      self->ready_error_ = error;
      self->ready_done_ = true;
      if (ok) {
        std::lock_guard queue_lock(self->queue_mutex_);
        self->render_running_ = true;
      }
    }
    self->ready_wake_.notify_all();
  });
  // Reports when queued work stops moving (a lost wake or GPU callback).
  emscripten_set_interval(WatchdogThunk, 5000.0, self);
  // WebGPU callbacks and drain requests arrive through the event loop.
  emscripten_exit_with_live_runtime();
  return nullptr;
}

uint32_t Gta4WebGpuGraphicsSystem::GetTitleCommandAbi(uint32_t title_id) const {
  return title_id == kTitleId && ready_ok_ ? kTitleCommandAbi : 0;
}

const uint8_t* Gta4WebGpuGraphicsSystem::GuestVirtual(uint32_t address, size_t size) const {
  if (!memory_ || !address || !size || uint64_t(address) + size > 0x100000000ull) return nullptr;
  return memory_->TranslateVirtual<const uint8_t*>(address);
}

const uint8_t* Gta4WebGpuGraphicsSystem::GuestPhysical(uint32_t address, size_t size) const {
  if (!memory_ || !size || uint64_t(address & 0x1FFFFFFFu) + size > 0x20000000ull) return nullptr;
  return memory_->TranslatePhysical<const uint8_t*>(address);
}

SurfaceDescriptor Gta4WebGpuGraphicsSystem::ReadSurface(uint32_t handle) const {
  SurfaceDescriptor out{};
  const uint8_t* bytes = handle ? GuestVirtual(handle, 44) : nullptr;
  if (!bytes) return out;
  out.handle = handle;
  out.flags = LoadBig32(bytes);
  out.base = LoadBig32(bytes + 24);
  out.address = LoadBig32(bytes + 28);
  out.packed_dimensions = LoadBig32(bytes + 36);
  out.format = LoadBig32(bytes + 40);
  out.width = (std::rotl(out.packed_dimensions, 14) & 0x3FFF) + 1;
  out.height = (std::rotl(out.packed_dimensions, 29) & 0x7FFF) + 1;
  out.sample_type = (out.base >> 16) & 3u;
  return out;
}

bool Gta4WebGpuGraphicsSystem::CaptureDevice(Work& work, uint32_t device, std::string& error) {
  const uint8_t* bytes = GuestVirtual(device, kGuestDeviceSize);
  if (!bytes) {
    error = "Unmapped guest device state";
    return false;
  }
  const double start = PerfNow();
  ++snapshots_;
  // The block changes on almost every draw, but usually only in a few
  // chunks; send those (all of them the first time).
  auto& sent = sent_devices_[device];
  const bool full = sent.empty();
  if (full) sent.resize(kGuestDeviceSize);
  work.device.device = device;
  work.device.bytes.reserve(full ? kGuestDeviceSize : 8 * kDeviceChunkBytes);
  for (uint32_t chunk = 0; chunk < kDeviceChunkCount; ++chunk) {
    const size_t offset = size_t(chunk) * kDeviceChunkBytes;
    if (!full && BytesEqual(sent.data() + offset, bytes + offset, kDeviceChunkBytes)) continue;
    std::memcpy(sent.data() + offset, bytes + offset, kDeviceChunkBytes);
    work.device.chunks[chunk / 32] |= 1u << (chunk % 32);
    work.device.bytes.insert(work.device.bytes.end(), bytes + offset,
                             bytes + offset + kDeviceChunkBytes);
  }
  snapshot_bytes_ += work.device.bytes.size();
  if (start) snapshot_ms_ += emscripten_get_now() - start;
  return ReadTargets(work, device, error);
}

bool Gta4WebGpuGraphicsSystem::ReadTargets(Work& work, uint32_t device, std::string& error) {
  const uint8_t* state = GuestVirtual(device, kGuestDeviceSize);
  if (!state) {
    error = "Unmapped guest device state";
    return false;
  }
  for (uint32_t i = 0; i < kRenderTargetCount; ++i)
    work.colors[i] = ReadSurface(LoadBig32(state + kDeviceColorTargets + i * 4));
  work.depth = ReadSurface(LoadBig32(state + kDeviceDepthTarget));
  return true;
}

std::shared_ptr<const BufferCapture> Gta4WebGpuGraphicsSystem::CaptureBuffer(
    uint32_t handle, std::string& error) {
  const uint8_t* header = GuestVirtual(handle, 32);
  if (!header) {
    error = "Unmapped guest buffer header";
    return {};
  }
  const uint32_t flags = LoadBig32(header);
  const auto metadata =
      DecodeNativeBufferMetadata(flags, LoadBig32(header + 24), LoadBig32(header + 28));
  if (!metadata || !metadata->HasValidPayload(kMaximumBufferBytes)) {
    error = "Invalid guest buffer metadata";
    return {};
  }
  struct Elapsed {
    double& total;
    double start;
    ~Elapsed() {
      if (start) total += emscripten_get_now() - start;
    }
  } elapsed{buffer_capture_ms_, PerfNow()};
  auto found = buffers_.find(handle);
  if (found != buffers_.end() && !metadata->guest_locked && !dirty_.contains(handle) &&
      found->second->address == metadata->guest_address &&
      found->second->bytes.size() == metadata->guest_size &&
      (found->second->flags & 0x8000000Fu) == (flags & 0x8000000Fu)) {
    return found->second;
  }
  const uint8_t* payload = GuestVirtual(metadata->guest_address, metadata->guest_size);
  if (!payload) {
    error = "Unmapped guest buffer payload";
    return {};
  }
  // An unchanged payload keeps its generation (and its converted GPU copies).
  if (found != buffers_.end() && found->second->bytes.size() == metadata->guest_size &&
      found->second->address == metadata->guest_address &&
      (found->second->flags & 0x8000000Fu) == (flags & 0x8000000Fu) &&
      BytesEqual(found->second->bytes.data(), payload, metadata->guest_size)) {
    dirty_.erase(handle);
    return found->second;
  }
  auto capture = std::make_shared<BufferCapture>();
  captured_bytes_ += metadata->guest_size;
  capture->generation = next_generation_++;
  capture->flags = flags;
  capture->address = metadata->guest_address;
  capture->bytes.assign(payload, payload + metadata->guest_size);
  buffers_[handle] = capture;
  dirty_.erase(handle);
  return capture;
}

std::shared_ptr<const TextureCapture> Gta4WebGpuGraphicsSystem::CaptureTexture(
    uint32_t handle, const xenos::xe_gpu_texture_fetch_t& fetch, std::string& error) {
  auto found = textures_.find(handle);
  if (found != textures_.end() && !dirty_.contains(handle) &&
      NativeTextureImageFetchEqual(found->second->fetch, fetch)) {
    return found->second;
  }
  struct Elapsed {
    double& total;
    double start;
    ~Elapsed() {
      if (start) total += emscripten_get_now() - start;
    }
  } elapsed{texture_capture_ms_, PerfNow()};
  TextureInfo info{};
  if (!TextureInfo::Prepare(fetch, &info) || info.mip_min_level > info.mip_max_level ||
      info.mip_max_level >= xenos::kTextureMaxMips) {
    error = "Invalid guest texture descriptor";
    return {};
  }
  const auto layout = texture_util::GetGuestTextureLayout(
      info.dimension, info.pitch >> 5, info.width + 1, info.height + 1, info.depth + 1,
      info.is_tiled, info.format, info.has_packed_mips, info.memory.base_address != 0,
      info.mip_max_level);
  auto capture = std::make_shared<TextureCapture>();
  capture->fetch = fetch;
  size_t total = 0;
  for (uint32_t level = info.mip_min_level; level <= info.mip_max_level; ++level) {
    uint32_t x = 0, y = 0;
    const uint32_t address = info.GetMipLocation(level, &x, &y, true);
    const auto& guest_level = level == 0 ? layout.base : layout.mips[level];
    const size_t size = guest_level.level_data_extent_bytes;
    const uint8_t* bytes = GuestPhysical(address, size);
    if (!bytes || !size || size > kMaximumTextureBytes - total) {
      error = "Unmapped or oversized guest texture level";
      return {};
    }
    total += size;
    captured_bytes_ += size;
    capture->mips.push_back({level, std::vector<uint8_t>(bytes, bytes + size)});
  }
  // Reloading identical bytes keeps the generation (no GPU re-upload).
  if (found != textures_.end() && NativeTextureImageFetchEqual(found->second->fetch, fetch) &&
      found->second->mips.size() == capture->mips.size() &&
      std::equal(capture->mips.begin(), capture->mips.end(), found->second->mips.begin(),
                 [](const auto& a, const auto& b) {
                   return a.bytes.size() == b.bytes.size() &&
                          BytesEqual(a.bytes.data(), b.bytes.data(), a.bytes.size());
                 })) {
    dirty_.erase(handle);
    return found->second;
  }
  capture->generation = next_generation_++;
  textures_[handle] = capture;
  dirty_.erase(handle);
  return capture;
}

bool Gta4WebGpuGraphicsSystem::CaptureDraw(Work& work, uint32_t device, bool indexed,
                                           std::string& error) {
  if (!CaptureDevice(work, device, error)) return false;
  const uint8_t* state = sent_devices_[device].data();
  const auto& bindings = devices_[device];
  const auto declaration = declarations_.find(bindings.declaration);
  if (declaration != declarations_.end()) {
    uint32_t streams = 0;
    for (const auto& element : declaration->second) streams |= 1u << element.stream;
    for (uint32_t stream = 0; stream < kVertexStreamCount; ++stream) {
      if (!(streams & (1u << stream))) continue;
      const uint32_t handle = LoadBig32(state + kDeviceVertexStreams + stream * 4);
      if (!handle) continue;
      work.streams[stream] = CaptureBuffer(handle, error);
      if (!work.streams[stream]) return false;
    }
  }
  if (indexed) {
    const uint32_t handle = LoadBig32(state + kDeviceIndexBuffer);
    if (!handle) {
      error = "Indexed draw has no index buffer";
      return false;
    }
    work.indices = CaptureBuffer(handle, error);
    if (!work.indices) return false;
  }
  uint32_t used = 0;
  for (uint32_t shader : {bindings.vertex_shader, bindings.pixel_shader}) {
    const auto found = shaders_.find(shader);
    if (found != shaders_.end() && found->second.record) used |= found->second.record->texture_mask;
  }
  for (uint32_t stage = 0; stage < kTextureStageCount; ++stage) {
    if (!(used & (1u << stage))) continue;
    const uint32_t handle = LoadBig32(state + kDeviceTextures + stage * 4);
    if (!handle || gpu_textures_.contains(handle)) continue;
    xenos::xe_gpu_texture_fetch_t fetch{};
    auto* words = reinterpret_cast<uint32_t*>(&fetch);
    for (size_t word = 0; word < 6; ++word)
      words[word] = LoadBig32(state + kDeviceFetchConstants + stage * 0x18 + word * 4);
    work.textures[stage] = CaptureTexture(handle, fetch, error);
    // An unsupported or unmapped texture leaves the stage unbound.
    if (!work.textures[stage]) {
      static uint32_t logged = 0;
      if (++logged <= 32)
        REXLOG_WARN("gta4-webgpu: texture {:08X} stage {} not captured: {}", handle, stage, error);
      error.clear();
    }
  }
  return true;
}

void Gta4WebGpuGraphicsSystem::ForgetResource(uint32_t handle) {
  buffers_.erase(handle);
  textures_.erase(handle);
  dirty_.erase(handle);
  gpu_textures_.erase(handle);
  shaders_.erase(handle);
  declarations_.erase(handle);
}

bool Gta4WebGpuGraphicsSystem::Capture(const void* command, size_t size, Work& work,
                                       std::string& error) {
  const auto header = Read<CommandHeader>(command);
  if (!TitleCommandSize(header.type) || header.size != size ||
      size != TitleCommandSize(header.type)) {
    error = "Invalid title command size/type";
    return false;
  }
  work.command.assign(static_cast<const std::byte*>(command),
                      static_cast<const std::byte*>(command) + size);
  switch (header.type) {
    case CommandType::kDeviceCreated: {
      const auto c = Read<DeviceCommand>(command);
      if (c.mode != 2) devices_[c.device] = {};
      sent_devices_.clear();
      return true;
    }
    case CommandType::kDeviceDestroyed:
      devices_.clear();
      buffers_.clear();
      textures_.clear();
      dirty_.clear();
      gpu_textures_.clear();
      shaders_.clear();
      declarations_.clear();
      sent_devices_.clear();
      return true;
    case CommandType::kRegisterShader: {
      const auto c = Read<RegisterShaderCommand>(command);
      if (!c.shader || !c.hash) {
        error = "Invalid shader registration";
        return false;
      }
      shaders_[c.shader] = {c.stage, archive_->Find(c.hash, c.stage)};
      return true;
    }
    case CommandType::kRegisterVertexDeclaration: {
      const auto c = Read<RegisterVertexDeclarationCommand>(command);
      if (!c.declaration || !c.element_count || c.element_count > kMaximumVertexElementCount) {
        error = "Invalid vertex declaration";
        return false;
      }
      for (uint32_t i = 0; i < c.element_count; ++i) {
        if (c.elements[i].stream >= kVertexStreamCount) {
          error = "Invalid vertex declaration element";
          return false;
        }
      }
      declarations_[c.declaration].assign(c.elements, c.elements + c.element_count);
      return true;
    }
    case CommandType::kSetPixelShader: {
      const auto c = Read<SetShaderCommand>(command);
      devices_[c.device].pixel_shader = c.shader;
      return true;
    }
    case CommandType::kSetVertexShader: {
      const auto c = Read<SetShaderCommand>(command);
      devices_[c.device].vertex_shader = c.shader;
      return true;
    }
    case CommandType::kSetVertexDeclaration: {
      const auto c = Read<SetVertexDeclarationCommand>(command);
      devices_[c.device].declaration = c.declaration;
      return true;
    }
    case CommandType::kResourceUnlock: {
      const auto c = Read<ResourceUnlockCommand>(command);
      if (c.access == ResourceUnlockAccess::kGuestWrite) {
        dirty_.insert(c.resource);
        // A guest write takes a virtual target back to CPU ownership.
        gpu_textures_.erase(c.resource);
      }
      return true;
    }
    case CommandType::kReleaseResource:
      ForgetResource(Read<ReleaseResourceCommand>(command).resource);
      return true;
    case CommandType::kRegisterVirtualResource: {
      const auto c = Read<RegisterVirtualResourceCommand>(command);
      if (c.kind == VirtualResourceKind::kTexture) gpu_textures_.insert(c.resource);
      textures_.erase(c.resource);
      return true;
    }
    case CommandType::kRegisterReflectionTarget: {
      const auto c = Read<RegisterReflectionTargetCommand>(command);
      if (c.texture) {
        gpu_textures_.insert(c.texture);
        textures_.erase(c.texture);
      }
      return true;
    }
    case CommandType::kDrawPrimitive: {
      const auto c = Read<DrawPrimitiveCommand>(command);
      return CaptureDraw(work, c.device, false, error);
    }
    case CommandType::kDrawIndexedPrimitive: {
      const auto c = Read<DrawIndexedPrimitiveCommand>(command);
      return CaptureDraw(work, c.device, true, error);
    }
    case CommandType::kDrawPrimitiveUp: {
      const auto c = Read<DrawPrimitiveUpCommand>(command);
      if (!c.vertex_data_size || c.vertex_data_size > kMaximumUpBytes) {
        error = "Invalid UP vertex payload extent";
        return false;
      }
      const uint8_t* payload = GuestVirtual(c.vertex_data, c.vertex_data_size);
      if (!payload) {
        error = "Unmapped UP vertex payload";
        return false;
      }
      work.up_vertices.assign(payload, payload + c.vertex_data_size);
      return CaptureDraw(work, c.device, false, error);
    }
    case CommandType::kClear:
      return ReadTargets(work, Read<ClearCommand>(command).device, error);
    case CommandType::kResolve: {
      const auto c = Read<ResolveCommand>(command);
      if (!c.source.handle || !c.destination_texture) {
        error = "Invalid resolve resource";
        return false;
      }
      gpu_textures_.insert(c.destination_texture);
      textures_.erase(c.destination_texture);
      dirty_.erase(c.destination_texture);
      if (c.device && !ReadTargets(work, c.device, error)) error.clear();
      return true;
    }
    case CommandType::kPresent: {
      const auto c = Read<PresentCommand>(command);
      if (c.frontbuffer_texture && !gpu_textures_.contains(c.frontbuffer_texture)) {
        work.present_source = CaptureTexture(
            c.frontbuffer_texture, std::bit_cast<xenos::xe_gpu_texture_fetch_t>(c.frontbuffer_fetch),
            error);
        if (!work.present_source) error.clear();
      }
      return true;
    }
    case CommandType::kTextureLock:
    case CommandType::kQueryDeviceCapabilities:
      error = "Command requires the synchronous title interface";
      return false;
    default:
      return true;
  }
}

void Gta4WebGpuGraphicsSystem::Enqueue(std::unique_ptr<Work> work, bool present) {
  {
    std::unique_lock lock(queue_mutex_);
    const auto has_space = [&] {
      return !render_running_ ||
             (queue_.size() < kMaximumQueuedCommands &&
              (!present || queued_presents_ < kMaximumQueuedPresents));
    };
    if (!has_space()) {
      const double start = emscripten_get_now();
      thread::wait_trace::Scope wait_scope("render queue full (%zu commands, %u presents)",
                                           queue_.size(), queued_presents_);
      queue_space_.wait(lock, has_space);
      blocked_ms_ += emscripten_get_now() - start;
    }
    if (!render_running_) return;
    if (present) ++queued_presents_;
    queue_.push_back(std::move(work));
  }
  ScheduleDrain();
}

void Gta4WebGpuGraphicsSystem::ScheduleDrain() {
  if (drain_scheduled_.exchange(true, std::memory_order_acq_rel)) return;
  if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(), render_thread_, DrainThunk,
                              this)) {
    drain_scheduled_.store(false, std::memory_order_release);
    REXLOG_ERROR("gta4-webgpu: render thread wake failed");
  }
}

void Gta4WebGpuGraphicsSystem::WatchdogThunk(void* self) {
  auto* system = static_cast<Gta4WebGpuGraphicsSystem*>(self);
  size_t queued;
  uint32_t presents;
  bool waiting;
  uint64_t executed;
  uint64_t presents_executed;
  uint32_t next_type;
  {
    std::lock_guard lock(system->queue_mutex_);
    queued = system->queue_.size();
    presents = system->queued_presents_;
    waiting = system->waiting_on_gpu_;
    executed = system->executed_;
    presents_executed = system->presents_executed_;
    next_type = queued ? uint32_t(system->queue_.front()->type()) : 0u;
  }
  const bool stalled = executed == system->watchdog_executed_ && (queued || waiting);
  system->watchdog_executed_ = executed;
  // No present for a while: report what every thread is blocked on (once per stall).
  if (presents_executed != system->watchdog_presents_) {
    system->watchdog_presents_ = presents_executed;
    system->watchdog_idle_ticks_ = 0;
  } else if (++system->watchdog_idle_ticks_ == 3 || system->watchdog_idle_ticks_ == 12) {
    thread::wait_trace::Dump(system->watchdog_idle_ticks_ == 3 ? "no present for 15 s"
                                                                : "no present for 60 s");
  }
  if (!stalled) return;
  REXLOG_WARN("gta4-webgpu: render queue stalled: {} commands ({} presents) queued, "
              "waiting on GPU={}, drain scheduled={}, yield pending={}, next command type={}, "
              "last type={}",
              queued, presents, waiting, system->drain_scheduled_.load(),
              system->yield_pending_, next_type, system->last_type_);
  // A lost wake or yield message leaves the queue idle; drain it from here.
  if (system->yield_pending_)
    ResumeThunk(system);
  else if (!waiting)
    system->Drain();
}

void Gta4WebGpuGraphicsSystem::DrainThunk(void* self) {
  auto* system = static_cast<Gta4WebGpuGraphicsSystem*>(self);
  // A wake that arrives during a yield is covered by the resume. Leaving
  // drain_scheduled_ set stops game threads from queueing more wakes, which
  // would keep this thread's mailbox (and so the current task) busy.
  if (system->yield_pending_) return;
  system->drain_scheduled_.store(false, std::memory_order_release);
  system->Drain();
}

void Gta4WebGpuGraphicsSystem::ResumeThunk(void* self) {
  auto* system = static_cast<Gta4WebGpuGraphicsSystem*>(self);
  system->yield_pending_ = false;
  system->drain_scheduled_.store(false, std::memory_order_release);
  system->Drain();
}

void Gta4WebGpuGraphicsSystem::YieldToEventLoop() {
  // The canvas shows a frame only when this worker's current task ends.
  // ScheduleDrain cannot end it: Emscripten runs tasks proxied to a thread
  // while it is executing that thread's mailbox in the same task, so with
  // work always queued the task never ended and the canvas froze. A
  // MessageChannel message is a new task.
  yield_pending_ = true;
  drain_scheduled_.store(true, std::memory_order_release);
  yield_resume = &ResumeThunk;
  yield_self = this;
  liberty_gpu_yield();
}

void Gta4WebGpuGraphicsSystem::Drain() {
  if (yield_pending_) return;
  for (;;) {
    std::unique_ptr<Work> work;
    {
      std::lock_guard lock(queue_mutex_);
      if (waiting_on_gpu_ || queue_.empty()) return;
      work = std::move(queue_.front());
      queue_.pop_front();
    }
    const bool present = work->type() == CommandType::kPresent;
    std::string error;
    const auto status = renderer_->Execute(*work, error);
    if (!error.empty()) {
      ++failures_;
      if (failures_ <= 64 || failures_ % 4096 == 0)
        REXLOG_ERROR("gta4-webgpu: command rejected type={} count={} reason={}",
                     uint32_t(work->type()), failures_, error);
    }
    {
      std::lock_guard lock(queue_mutex_);
      ++executed_;
      last_type_ = uint32_t(work->type());
      if (present) {
        --queued_presents_;
        ++presents_executed_;
      }
      if (status == Renderer::Status::kPending) waiting_on_gpu_ = true;
    }
    queue_space_.notify_all();
    if (status == Renderer::Status::kPending) return;
    if (status == Renderer::Status::kYield) {
      // Return to the event loop so the canvas can show the frame.
      YieldToEventLoop();
      return;
    }
  }
}

bool Gta4WebGpuGraphicsSystem::SubmitTitleCommand(uint32_t title_id, uint32_t abi,
                                                  const void* command, size_t size) {
  if (title_id != kTitleId) return false;
  if (!Unwrap(abi, command, size)) {
    static std::atomic<uint32_t> logged{0};
    if (++logged <= 16) REXLOG_WARN("gta4-webgpu: cannot unwrap title command abi={:08X}", abi);
    return false;
  }
  const auto header = Read<CommandHeader>(command);
  if (header.type == CommandType::kTemporalUpdate) return true;  // No temporal AA here.
  const bool present = header.type == CommandType::kPresent;
  if (present) PacePresent();
  auto work = std::make_unique<Work>();
  std::string error;
  // Queued under the capture lock: device deltas must reach the render
  // thread in the order they were taken.
  std::lock_guard lock(capture_mutex_);
  const double start = PerfNow();
  const bool captured = Capture(command, size, *work, error);
  if (start) capture_ms_ += emscripten_get_now() - start;
  if (present && REXCVAR_GET(webgpu_perf_report)) {
    const double now = emscripten_get_now();
    if (!report_start_ms_) report_start_ms_ = now;
    if (now - report_start_ms_ >= 5000.0) {
      double blocked;
      {
        std::lock_guard queue_lock(queue_mutex_);
        blocked = blocked_ms_;
        blocked_ms_ = 0;
      }
      WEBGPU_PERF_LOG("gta4-webgpu: capture over {:.0f} ms: {:.0f} ms capturing (device {:.0f} "
                      "for {} draws, {} KB sent; buffers {:.0f}, textures {:.0f}; {} KB "
                      "copied), {:.0f} ms waiting on the render thread",
                      now - report_start_ms_, capture_ms_, snapshot_ms_, snapshots_,
                      snapshot_bytes_ / 1024, buffer_capture_ms_, texture_capture_ms_,
                      captured_bytes_ / 1024, blocked);
      capture_ms_ = snapshot_ms_ = buffer_capture_ms_ = texture_capture_ms_ = 0;
      captured_bytes_ = snapshots_ = snapshot_bytes_ = 0;
      report_start_ms_ = now;
    }
  }
  if (!captured) {
    // The render thread never sees this delta; send the next one in full.
    if (work->device.device) sent_devices_.erase(work->device.device);
    ++failures_;
    if (failures_ <= 64 || failures_ % 4096 == 0)
      REXLOG_ERROR("gta4-webgpu: capture rejected type={} count={} reason={}",
                   uint32_t(header.type), failures_, error);
    return false;
  }
  Enqueue(std::move(work), present);
  return true;
}

void Gta4WebGpuGraphicsSystem::PacePresent() {
  // Nothing else limits the frame rate: the canvas does not block on vsync,
  // and Node has no canvas. Loading screens otherwise present thousands of
  // frames a second, which only burns CPU the streaming threads need.
  const uint32_t limit = REXCVAR_GET(webgpu_frame_limit);
  if (!limit) return;
  const double interval = 1000.0 / limit;
  double now = emscripten_get_now();
  if (now < next_present_ms_) {
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(next_present_ms_ - now));
    now = emscripten_get_now();
  }
  // A frame that ran late starts a new schedule rather than being made up.
  next_present_ms_ = (now - next_present_ms_ > interval ? now : next_present_ms_) + interval;
}

bool Gta4WebGpuGraphicsSystem::ExecuteTitleCommand(uint32_t title_id, uint32_t abi,
                                                   const void* command, size_t size, void* result,
                                                   size_t result_size) {
  if (title_id != kTitleId || !result || !Unwrap(abi, command, size)) return false;
  const auto header = Read<CommandHeader>(command);
  if (header.size != size || size != TitleCommandSize(header.type)) return false;
  if (header.type == CommandType::kQueryDeviceCapabilities &&
      result_size == sizeof(DeviceCapabilitiesResult)) {
    const DeviceCapabilitiesResult capabilities{renderer_->max_texture_dimension(), 0};
    std::memcpy(result, &capabilities, sizeof(capabilities));
    return true;
  }
  if (header.type != CommandType::kTextureLock || result_size != sizeof(TextureLockResult))
    return false;
  if (pthread_equal(pthread_self(), render_thread_)) return false;  // Would deadlock.
  auto work = std::make_unique<Work>();
  work->command.assign(static_cast<const std::byte*>(command),
                       static_cast<const std::byte*>(command) + size);
  auto slot = std::make_shared<ExecuteSlot>();
  work->execute = slot;
  Enqueue(std::move(work), false);
  std::unique_lock lock(slot->mutex);
  thread::wait_trace::Scope wait_scope("synchronous title command %u", uint32_t(header.type));
  slot->wake.wait(lock, [&] { return slot->done || !render_running_; });
  if (!slot->done || !slot->success || slot->result.size() != result_size) {
    if (!slot->error.empty())
      REXLOG_ERROR("gta4-webgpu: synchronous command rejected: {}", slot->error);
    return false;
  }
  std::memcpy(result, slot->result.data(), result_size);
  return true;
}

void Gta4WebGpuGraphicsSystem::Shutdown() {
  {
    std::lock_guard lock(queue_mutex_);
    render_running_ = false;
  }
  queue_space_.notify_all();
  // The render worker idles in its event loop and ends with the runtime.
}

}  // namespace rex::graphics::gta4_webgpu

#include <rex/graphics/gta4_webgpu.h>

namespace rex::graphics::gta4_webgpu {
std::unique_ptr<system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<Gta4WebGpuGraphicsSystem>();
}
}  // namespace rex::graphics::gta4_webgpu
