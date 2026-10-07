#pragma once

#include <cstdint>

namespace rex::graphics::gta4_webgpu {

// The page's <canvas id="liberty-gpu">, as seen from the render thread.
inline constexpr char kCanvasSelector[] = "#liberty-gpu";

// Asks the page to transfer the canvas to the calling (render) thread. A
// no-op outside a browser page.
void RequestCanvas();
// True once the canvas has arrived; reports its current size in pixels.
bool PollCanvas(uint32_t& width, uint32_t& height);

}  // namespace rex::graphics::gta4_webgpu
