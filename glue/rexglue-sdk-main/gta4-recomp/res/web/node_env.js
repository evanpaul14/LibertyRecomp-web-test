// Pre-js for the Node.js test build (LIBERTY_WEB_NODERAWFS=ON).
// Emscripten does not expose the host environment to getenv(); forward the
// variables that choose where the title keeps its data (XDG_DATA_HOME/HOME,
// see rex::filesystem) so a run can point at a local game directory.
if (ENVIRONMENT_IS_NODE) {
  Module['preRun'] = [].concat(Module['preRun'] || [], () => {
    for (const name of ['XDG_DATA_HOME', 'HOME']) {
      if (process.env[name]) ENV[name] = process.env[name];
    }
  });
}

// Headless WebGPU for Node test runs: LIBERTY_DAWN_NODE names the Dawn
// package (npm "webgpu"). Pre-js code runs in every worker, and WebGPU
// objects live in the worker that creates them, so each gets its own GPU.
if (ENVIRONMENT_IS_NODE && process.env.LIBERTY_DAWN_NODE && !globalThis.navigator?.gpu) {
  const dawn = require(process.env.LIBERTY_DAWN_NODE);
  Object.assign(globalThis, dawn.globals);
  if (!globalThis.navigator) globalThis.navigator = {};
  Object.defineProperty(globalThis.navigator, 'gpu', { value: dawn.create([]), configurable: true });
}
