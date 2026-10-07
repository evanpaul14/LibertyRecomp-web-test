#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "work.h"

namespace rex::memory {
class Memory;
}

namespace rex::graphics::gta4_webgpu {

class ShaderArchive;

// Executes captured title commands with WebGPU. Every method runs on the
// render thread, which owns all WebGPU objects.
class Renderer {
 public:
  enum class Status {
    kDone,     // Finished; keep draining.
    kYield,    // Finished; return to the event loop first (a frame was presented).
    kPending,  // Waiting on the GPU (a readback); the resume callback continues.
  };

  Renderer(memory::Memory* memory, const ShaderArchive* archive);
  ~Renderer();

  // Creates the adapter and device. `done` runs on the render thread.
  void Initialize(std::function<void(bool ok, const std::string& error)> done);
  // Called on the render thread once a kPending command has completed.
  void SetResume(std::function<void()> resume);
  Status Execute(Work& work, std::string& error);
  uint32_t max_texture_dimension() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex::graphics::gta4_webgpu
