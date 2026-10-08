#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/graphics/gta4_native/title_commands.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::gta4_webgpu {

// Guest bytes copied on the submitting thread. The render thread runs later,
// so everything a command reads from guest memory is captured with it.
struct BufferCapture {
  uint64_t generation = 0;
  uint32_t flags = 0;
  uint32_t address = 0;
  std::vector<uint8_t> bytes;  // Big-endian, as in guest memory.
};

struct TextureCapture {
  struct Mip {
    uint32_t level = 0;
    std::vector<uint8_t> bytes;  // Physical guest bytes of the level.
  };
  uint64_t generation = 0;
  xenos::xe_gpu_texture_fetch_t fetch{};
  std::vector<Mip> mips;
};

// Result slot of a synchronous command. The submitting thread waits on it.
struct ExecuteSlot {
  std::mutex mutex;
  std::condition_variable wake;
  bool done = false;
  bool success = false;
  std::vector<std::byte> result;
  std::string error;

  void Finish(bool ok) {
    {
      std::lock_guard lock(mutex);
      success = ok;
      done = true;
    }
    wake.notify_all();
  }
};

// The guest device block is sent to the render thread as the chunks that
// changed since the previous draw on that device; it keeps its own copy.
inline constexpr uint32_t kDeviceChunkBytes = 128;
inline constexpr uint32_t kDeviceChunkCount = gta4_native::kGuestDeviceSize / kDeviceChunkBytes;
static_assert(gta4_native::kGuestDeviceSize % kDeviceChunkBytes == 0);

struct DeviceDelta {
  uint32_t device = 0;  // Guest address of the device; 0 when the command has none.
  std::array<uint32_t, (kDeviceChunkCount + 31) / 32> chunks{};  // Changed chunks.
  std::vector<uint8_t> bytes;  // Their contents, in chunk order.
};

struct Work {
  std::vector<std::byte> command;
  DeviceDelta device;  // Draws only.
  std::array<gta4_native::SurfaceDescriptor, gta4_native::kRenderTargetCount> colors{};
  gta4_native::SurfaceDescriptor depth{};
  std::array<std::shared_ptr<const BufferCapture>, gta4_native::kVertexStreamCount> streams{};
  std::shared_ptr<const BufferCapture> indices;
  std::array<std::shared_ptr<const TextureCapture>, gta4_native::kTextureStageCount> textures{};
  std::shared_ptr<const TextureCapture> present_source;  // CPU frontbuffer.
  std::vector<uint8_t> up_vertices;                       // DrawPrimitiveUp payload.
  std::shared_ptr<ExecuteSlot> execute;                   // Synchronous commands.

  gta4_native::CommandType type() const {
    gta4_native::CommandHeader header{};
    std::memcpy(&header, command.data(), sizeof(header));
    return header.type;
  }
  template <class T>
  T As() const {
    T value{};
    std::memcpy(&value, command.data(), sizeof(value));
    return value;
  }
};

}  // namespace rex::graphics::gta4_webgpu
