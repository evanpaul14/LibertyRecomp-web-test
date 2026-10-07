// Pre-js for the web build's WebGPU renderer. Its render thread asks the page
// for <canvas id="liberty-gpu"> (src/graphics/gta4_webgpu/canvas.cpp); the
// page transfers it to that thread's worker as an OffscreenCanvas, followed
// by size updates. Messages without a `cmd` are ignored by Emscripten.
if (typeof WorkerGlobalScope != 'undefined' && self instanceof WorkerGlobalScope) {
  self.addEventListener('message', (event) => {
    const data = event.data;
    if (!data) return;
    if (data.libertyGpuCanvas) globalThis.libertyGpuCanvas = data.libertyGpuCanvas;
    if (data.libertyGpuCanvasSize) globalThis.libertyGpuCanvasSize = data.libertyGpuCanvasSize;
  });
}
