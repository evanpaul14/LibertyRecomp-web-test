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

struct Work {
  std::vector<std::byte> command;
  // Device block (kGuestDeviceSize bytes) for draws, clears and resolves.
  std::shared_ptr<const std::vector<uint8_t>> device;
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
