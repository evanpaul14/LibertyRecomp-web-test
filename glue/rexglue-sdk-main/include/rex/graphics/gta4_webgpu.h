#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace rex::graphics::gta4_webgpu {

// GTA IV's title-command renderer on WebGPU. Web (Emscripten) builds only;
// statically linked, since the web build has no GPU plugin loader.
std::unique_ptr<system::IGraphicsSystem> CreateGraphicsSystem();

}  // namespace rex::graphics::gta4_webgpu
