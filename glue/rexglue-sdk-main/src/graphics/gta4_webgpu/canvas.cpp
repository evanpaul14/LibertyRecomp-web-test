#include "canvas.h"

#include <emscripten/emscripten.h>
#include <pthread.h>

namespace rex::graphics::gta4_webgpu {
namespace {
// Runs on the render thread. The page's canvas arrives as an OffscreenCanvas
// message (gta4-recomp/res/web/webgpu_canvas.js); expose it to the WebGPU
// bindings under kCanvasSelector and apply any pending resize.
EM_JS(int, liberty_gpu_canvas_poll, (), {
  var canvas = globalThis.libertyGpuCanvas;
  if (!canvas) return 0;
  var size = globalThis.libertyGpuCanvasSize;
  if (size) {
    canvas.width = size[0];
    canvas.height = size[1];
    globalThis.libertyGpuCanvasSize = null;
  }
  specialHTMLTargets['#liberty-gpu'] = canvas;
  return (Math.min(canvas.width, 32767) << 16) | Math.min(canvas.height, 65535);
});
}  // namespace

void RequestCanvas() {
  // On the page's main thread: hand the WebGPU canvas to this thread's worker.
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        if (typeof document == 'undefined') return;
        var canvas = document.getElementById('liberty-gpu');
        var worker = PThread.pthreads[$0];
        if (!canvas || !worker || !canvas.transferControlToOffscreen) return;
        // (No bare commas: EM_ASM is a macro.)
        var size = () => {
          var ratio = globalThis.devicePixelRatio || 1;
          var result = new Array(2);
          result[0] = Math.max(1, Math.round(canvas.clientWidth * ratio));
          result[1] = Math.max(1, Math.round(canvas.clientHeight * ratio));
          return result;
        };
        var initial = size();
        canvas.width = initial[0];
        canvas.height = initial[1];
        var offscreen = canvas.transferControlToOffscreen();
        worker.postMessage({libertyGpuCanvas: offscreen}, [offscreen]);
        new ResizeObserver(() => worker.postMessage({libertyGpuCanvasSize: size()})).observe(canvas);
      },
      double(uintptr_t(pthread_self())));
}

bool PollCanvas(uint32_t& width, uint32_t& height) {
  const int packed = liberty_gpu_canvas_poll();
  if (!packed) return false;
  width = uint32_t(packed) >> 16;
  height = uint32_t(packed) & 0xFFFF;
  return width && height;
}

}  // namespace rex::graphics::gta4_webgpu
