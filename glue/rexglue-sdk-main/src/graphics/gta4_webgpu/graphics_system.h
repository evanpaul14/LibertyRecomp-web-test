#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pthread.h>

#include <rex/system/interfaces/graphics.h>

#include "work.h"

namespace rex::memory {
class Memory;
}

namespace rex::graphics::gta4_webgpu {

class Renderer;
class ShaderArchive;
struct ShaderRecord;

// GTA IV's title-command renderer on WebGPU (web builds only).
//
// WebGPU objects belong to the JavaScript worker that created them, and
// buffer mapping and canvas presentation need that worker to return to its
// event loop. A dedicated render thread therefore owns the device and runs
// from the event loop. The game's threads capture what each command reads
// from guest memory (device state, buffers, textures) and queue it.
class Gta4WebGpuGraphicsSystem final : public system::IGraphicsSystem {
 public:
  Gta4WebGpuGraphicsSystem();
  ~Gta4WebGpuGraphicsSystem() override;

  X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                         system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presentation_ready_; }
  uint32_t GetTitleCommandAbi(uint32_t title_id) const override;
  bool SubmitTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command,
                          size_t command_size) override;
  bool ExecuteTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command,
                           size_t command_size, void* result, size_t result_size) override;
  void Shutdown() override;

 private:
  struct ShaderBinding {
    gta4_native::ShaderStage stage{};
    const ShaderRecord* record = nullptr;
  };
  struct DeviceBindings {
    uint32_t vertex_shader = 0;
    uint32_t pixel_shader = 0;
    uint32_t declaration = 0;
  };

  // Producer side, under capture_mutex_.
  bool Capture(const void* command, size_t size, Work& work, std::string& error);
  bool CaptureDraw(Work& work, uint32_t device, bool indexed, std::string& error);
  bool SnapshotDevice(Work& work, uint32_t device, std::string& error);
  std::shared_ptr<const BufferCapture> CaptureBuffer(uint32_t handle, std::string& error);
  std::shared_ptr<const TextureCapture> CaptureTexture(uint32_t handle,
                                                       const xenos::xe_gpu_texture_fetch_t& fetch,
                                                       std::string& error);
  gta4_native::SurfaceDescriptor ReadSurface(uint32_t handle) const;
  const uint8_t* GuestVirtual(uint32_t address, size_t size) const;
  const uint8_t* GuestPhysical(uint32_t address, size_t size) const;
  void ForgetResource(uint32_t handle);
  void Enqueue(std::unique_ptr<Work> work, bool present);
  void PacePresent();

  // Render thread.
  static void* RenderThreadMain(void* self);
  static void DrainThunk(void* self);
  static void WatchdogThunk(void* self);
  void Drain();
  void ScheduleDrain();

  memory::Memory* memory_ = nullptr;
  const ShaderArchive* archive_ = nullptr;
  bool presentation_ready_ = false;

  std::mutex capture_mutex_;
  std::unordered_map<uint32_t, ShaderBinding> shaders_;
  std::unordered_map<uint32_t, DeviceBindings> devices_;
  std::unordered_map<uint32_t, std::vector<gta4_native::VertexElement>> declarations_;
  std::unordered_map<uint32_t, std::shared_ptr<const BufferCapture>> buffers_;
  std::unordered_map<uint32_t, std::shared_ptr<const TextureCapture>> textures_;
  std::unordered_set<uint32_t> dirty_;
  // Textures whose contents the GPU produces (resolve destinations, virtual
  // and reflection targets): never captured from guest memory.
  std::unordered_set<uint32_t> gpu_textures_;
  uint64_t next_generation_ = 1;
  // Submitting-thread time (ms) since the last report, under capture_mutex_
  // except blocked_ms_ (under queue_mutex_).
  double capture_ms_ = 0, snapshot_ms_ = 0, buffer_capture_ms_ = 0, texture_capture_ms_ = 0, report_start_ms_ = 0;
  uint64_t captured_bytes_ = 0, snapshots_ = 0, snapshot_copies_ = 0;
  double blocked_ms_ = 0;
  double next_present_ms_ = 0;  // Earliest time of the next present (presenting thread).
  std::shared_ptr<std::vector<uint8_t>> last_device_;
  uint32_t last_device_address_ = 0;

  std::mutex queue_mutex_;
  std::condition_variable queue_space_;
  std::deque<std::unique_ptr<Work>> queue_;
  uint32_t queued_presents_ = 0;
  std::atomic<bool> drain_scheduled_{false};
  bool render_running_ = false;
  bool waiting_on_gpu_ = false;
  uint64_t executed_ = 0;  // Commands the render thread has run (under queue_mutex_).
  uint32_t last_type_ = 0;
  uint64_t presents_executed_ = 0;  // Under queue_mutex_.
  uint64_t watchdog_executed_ = 0;  // Render thread only.
  uint64_t watchdog_presents_ = 0;  // Render thread only.
  uint32_t watchdog_idle_ticks_ = 0;  // Render thread only.

  pthread_t render_thread_{};
  bool render_thread_started_ = false;
  std::mutex ready_mutex_;
  std::condition_variable ready_wake_;
  bool ready_done_ = false;
  bool ready_ok_ = false;
  std::string ready_error_;
  std::unique_ptr<Renderer> renderer_;
  uint64_t failures_ = 0;
};

}  // namespace rex::graphics::gta4_webgpu
