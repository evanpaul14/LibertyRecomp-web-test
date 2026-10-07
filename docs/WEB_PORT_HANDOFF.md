# Web Port Handoff

Status of the WebAssembly port on branch `webgpu-node-graphics` (branched from
`main`; earlier work was on `claude/zen-goodall-jpl0op`).
`WEB_BUILDING.md` is the user-facing guide (build, run, how each subsystem is
mapped); this file is for whoever continues the work.

## Where it stands

- The RexGlue runtime, FFmpeg and all 89 generated GTA IV translation units
  build to **wasm64** (Memory64, pthreads, wasm EH, SIMD128) and link into
  `out/web/LibertyRecomp/LibertyRecomp.{js,wasm}` (about 116 MB, 12 MB of it the
  embedded WGSL shader archive).
- In headless Chromium 141 the page is cross-origin isolated and the app
  starts and reaches the install check (there is no way to give it game files
  in a browser yet).
- **WebGPU renderer (this session's work).** A title-command renderer
  (`src/graphics/gta4_webgpu`) replaces headless mode. Under Node 24 with
  Dawn (npm `webgpu`) on SwiftShader and real game files (USA retail 1.00 disc
  + title update 8), the game draws its loading screen, starts a new game and
  renders Liberty City (night skyline, water reflections, HUD radar) at about
  8000 draws a frame with **no rejected draws and no WebGPU errors**. It takes
  over 15 minutes on SwiftShader to get there; the furthest frame reached is
  2760 (a 25-minute run cap, not a crash).
- **Measured speed (SwiftShader, 4 cores, from dump file times):** the loading
  screen runs at about 24 fps; gameplay frames take 4–7 s each. The slowest
  stretch (frames 2520→2640, ~6.7 s/frame) had only ~500 draws a frame while
  pipelines grew 47→159 and textures 85→~2400, which points at one-time costs
  (synchronous pipeline compiles, which SwiftShader turns into CPU code, and
  texture decoding on the render thread) more than per-draw cost. Not profiled
  yet; SwiftShader and the game also share the same 4 CPU cores.
- All 2062 title shader variants translate to WGSL (`tools/webgpu`) and pass
  Tint validation; the archive is checked in and embedded.
- Without game files the browser build stops at the install check. With
  `tools/web/serve.py` serving an installed game (`/game/` + manifest, mounted
  lazily by `res/web/index.html`), it reaches gameplay (see below).
- `rex-web-memory-test` (26 checks: alias folding, byte order, heaps, MMIO,
  returned-pointer aliases) passes under Node 24.
- **Real browser (Chrome on an M1 Mac, 2026-10-07).** With game files served by
  `tools/web/serve.py` (installed from a disc image extracted to a folder; the
  installer cannot map a 7.8 GB `.iso` under Node), the build reaches gameplay
  in Chrome. An earlier run showed black gameplay at about 5 fps; it predated
  the clock, hang, stencil-rebuild and downsample-resolve fixes below.
  - *Rechecked with those fixes:* the intro cutscenes render lit and correct
    (ship's cabin, the docks meeting with the in-world credits) with no stalls
    or GPU errors. Light scenes (400–700 draws) run at 40–59 fps; the heaviest
    stretch (~6,000–6,600 draws) dropped to 3–6 fps, at ~20–32 µs of
    render-thread time per draw, spread over inputs, bindings, uniforms and
    encoding (pipeline compiles were near zero). Gameplay proper (11–15k
    draws) has not been reached in Chrome since.
  - *Fixed: frozen canvas.* The picture stopped updating once a heavy scene
    put the renderer behind the game, while rendering went on. A worker's
    canvas shows a frame only when its current task ends, and the drain's
    "yield" after a present re-proxied itself to the render thread, which
    Emscripten runs in the same task while it executes that thread's mailbox
    (`em_task_queue_execute`). With work always queued, the task never ended.
    The drain now resumes from a `MessageChannel` message (a new task), with
    game-thread wakes suppressed until then (`YieldToEventLoop` in
    `graphics_system.cpp`). Found with the new `presents` perf line (below):
    every present was shown with new content, yet the picture was frozen.
  - The Claude-in-Chrome extension could not see the WebGPU canvas in its own
    tab group (it stayed dark there while a normal tab rendered); use a tab
    opened normally (`open -a "Google Chrome" <url>`) and a screen capture.
- **Profiling.** `--webgpu_perf_report=true` logs render-thread and capture
  timing every 5 s as warnings, plus a `presents` line: presents shown on the
  canvas, those with new frontbuffer contents, those skipped (no source, no
  canvas, no surface texture with its status), canvas size, frontbuffer and GPU
  error count. Use it with `--diagnostics=true
  --diagnostics_categories=logging --log_level=warn`: other diagnostics print
  a line per draw, and every line blocks the game thread until the page's
  main thread handles it. In the browser that alone held gameplay below 1 fps.
- Next step candidates, in suggested order: per-draw render-thread cost
  (the heavy cutscene stretch runs at 3–6 fps; see the Chrome numbers above),
  confirming Chrome through gameplay proper, then a game-file picker. Ask the
  user.
- **Node graphics session (Dawn on Metal, M1 Mac, 2026-10-07).**
  - *Fixed: black loading screens.* Emscripten has no `CLOCK_MONOTONIC_RAW`;
    `clock_getres`/`clock_gettime` failed and, with asserts compiled out, the
    host clock was uninitialized memory. Guest frame deltas were ~8e8 s, so the
    loading screens stayed behind their opaque fade quad (and the game's timers
    were garbage everywhere). `src/core/clock_posix.cpp` now uses
    `CLOCK_MONOTONIC` on Emscripten. This bug also affected the browser build,
    which likely explains the missing loading artwork seen in Chrome.
  - *Added:* present pacing (`--webgpu_frame_limit`, default 60) and a richer
    frame trace (see Testing below).
  - *Fixed: hang.* Node runs used to stop presenting after about 3.5 minutes.
    The main game thread was stuck in `XamInputGetState` →
    `MnkInputDriver::UpdateMouseCapture` → `CallInUIThreadSynchronous`, waiting
    for a UI-thread wakeup that never ran. SDL3 was built without threads on
    Emscripten (`SDL_THREADS_DISABLED`, its default there), so its mutexes did
    nothing and game threads pushing wakeup events raced the UI thread on the
    event queue. Relative mouse mode always fails on the web, so capture was
    retried (one synchronous round trip plus an error line) on every input
    poll, which made the race frequent. Fixes: `SDL_PTHREADS=ON` for Emscripten
    (`thirdparty/CMakeLists.txt`), and a failed capture is not retried until
    focus returns (`mnk_input_driver.cpp`). A 19-minute run reached gameplay
    with no stall. The suspend/APC theory was wrong: no thread suspends happen.
  - *Hang diagnostics.* `rex/thread/wait_trace.h` (web only) records what each
    thread is blocked on (kernel waits, critical sections, delays, suspends,
    render-queue and synchronous-command waits). The render-thread watchdog logs
    the list as warnings after 15 s and 60 s without a present. For a thread
    shown as "running (or blocked outside a traced wait)", get its wasm stack
    from the live process: build with `LIBERTY_WEB_FUNCTION_NAMES=ON`, run Node
    with `--inspect` (or `kill -USR1` it), then use the inspector's
    `NodeWorker` domain to send `Debugger.pause` to each worker and read
    `callFrames`.
  - *Fixed: black deferred lighting.* After the G-buffer, the title hands the
    scene depth to the forward depth surface with `kRebuildSceneCoverage`: stencil
    must become 0x80 where the scene is empty and 0xFF where it is covered. The
    WebGPU handoff only copied depth, so the full-screen lighting pass (stencil
    `Equal 0x01`, mask 0x01) was rejected everywhere and lit surfaces stayed
    black. `Handoff` (`passes.cpp`) now follows the Metal renderer: depth comes
    from the resolved snapshot (`source_texture`), and a rebuild clears stencil
    to 0x80 and writes 0xFF with a fixed-function stencil Replace where packed
    depth is nonzero (no shader stencil export needed). The intro cutscenes now
    render lit. The draw trace also logs stencil state, and handoffs are traced.
  - *Fixed: hard white lamp shapes.* These were not coronas (the corona shader
    62DFF2DBDC8ED5D6 adds soft glows correctly). The bloom/exposure chain
    downsamples by resolving a 4x MSAA view (e.g. 512x384) of a 1x surface's
    EDRAM (1024x768); the resolve picked the 1x surface and copied its top-left
    quarter unscaled, so bloom held the frame's top-left quarter at 2x and lit
    a ghost of each lamp at twice its screen position, over anything in front.
    `Resolve` now maps the requested view's samples onto the owner surface and
    averages them (`resolve_color`, as `gta4_native/resolve_convert_ps.glsl`).
    Found with `--webgpu_trace_pixel`, which lists the draws that changed a texel.
  - *Packed depth aliases:* `RegisterVirtualResource` with `packed_depth_source`
    is honored; resolved depth is stored as `rg32float` (depth, stencil) and an
    alias texture is rebuilt as A8R8G8B8 like
    `gta4_native/packed_depth_alias_ps.glsl`. The title does take this path: the
    full-screen lighting draw samples one (an RGBA8 GPU texture in slot 5 that no
    resolve writes). Its contents have not been checked against Metal.
  - *Not checked:* in the overhead shot of the ship's hold (around frame 6240)
    large areas are black around the characters; probably just an unlit hold,
    unverified. One texture (read by a full-screen pass whose output is never
    resolved) is never produced; harmless so far.
  - *Render-thread watchdog:* every 5 s it logs `render queue stalled` (and drains
    the queue) when queued work stops moving, and dumps the wait trace after 15 s
    and 60 s without a present.

## Key design decisions (and where they live)

| Topic | Decision | Files |
|------|----------|-------|
| Toolchain | Flags must be set before `Emscripten.cmake` is included, or CMake picks wasm32 | `toolchains/web-emscripten.cmake` |
| Platform macro | `REX_PLATFORM_WEB` also sets `REX_PLATFORM_LINUX`; `REX_ARCH_WASM64` | `include/rex/platform.h` |
| Guest memory | One `sbrk` region `[virtual 4 GB][physical 512 MB]`, zero by construction. Aliases at `0x7F`/`0xA0`/`0xC0`/`0xE0` are folded onto the physical copy via a 256-entry table. `0x90` is *not* folded | `include/rex/memory/web_guest_layout.h`, `src/core/memory_web.cpp`, `src/system/xmemory.cpp` |
| Generated-code access | On web, `REX_LOAD/STORE_*` call helpers that fold aliases and route the `0x7F` block to `MMIOHandler::CheckLoad/CheckStore`. The change is in the header **and** the codegen template, so no game code had to be regenerated | `gta4-recomp/generated/gta4_init.h`, `resources/templates/codegen/init_h.inja`, `include/rex/system/web_guest_access.h` |
| Native helper returns | Returned host pointers take the alias of the nearest argument (this fixed the shader-preload failure) | `include/rex/ppc/function.h` (`detail::HostPointerToGuest`) |
| FPSCR | Per-thread virtual control word; wasm always rounds to nearest | `include/rex/platform/fpscr.h` |
| Fibers | Thread fibers only; `Create`/`SwitchTo` fail loudly (GTA IV uses none) | `src/core/fiber_web.cpp` |
| GPU | Statically linked WebGPU title-command renderer (no `dlopen` on web); created in `GTA4App::OnPreSetup`. `--gpu_plugin=none` = headless | `src/graphics/gta4_webgpu/`, `include/rex/graphics/gta4_webgpu.h`, `gta4-recomp/src/gta4_app.cpp` |
| GPU threading | One render pthread owns the device and runs from the JS event loop (needed for `mapAsync` and canvas presentation), woken via `emscripten_proxy_async`; after each present it yields with a `MessageChannel` message, since a proxied wake can run in the same task and the canvas only updates when the task ends. Game threads capture device block/buffers/textures at submit (`Capture` in `graphics_system.cpp`) | `gta4_webgpu/graphics_system.cpp`, `work.h` |
| Shaders | Stock SPIR-V → GLSL (SPIRV-Cross) → rewrite BDA constants to one UBO (VS 0, PS 4096, shared 8192, spec word 8192+0x500) and bindless to fixed slots → glslang → spirv-opt → naga → WGSL. naga undoes the Vulkan y-flip itself | `tools/webgpu/spirv_to_wgsl.py`, `LibertyRecompLib/shader/webgpu_shader_archive.bin` |
| Vertex data | Every attribute decoded to `float32x4` on the CPU per buffer generation (shaders read vec4 floats; WebGPU cannot feed integer formats to them) | `gta4_webgpu/vertex_decode.h`, `resources.cpp` |
| Render targets | Single-sampled; resolved depth stored as `rg32float` (depth, stencil), with packed A8R8G8B8 aliases rebuilt on demand; resolves pick the latest surface at the same EDRAM placement, and map samples when its MSAA layout differs from the resolved view (`resolve_color`) | `gta4_webgpu/resources.cpp`, `passes.cpp`, `renderer.cpp` |
| Depth handoff | Depth from the resolved snapshot (`source_texture`); `kRebuildSceneCoverage` clears stencil to 0x80 and writes 0xFF via stencil Replace where packed depth is nonzero, as the Metal renderer | `gta4_webgpu/passes.cpp` (`Handoff`) |
| Canvas | `<canvas id="liberty-gpu">` is transferred to the render worker as an OffscreenCanvas (pre-js `res/web/webgpu_canvas.js`); SDL's `#canvas` stays on top for input | `gta4_webgpu/canvas.cpp`, `res/web/index.html` |
| Main loop | `-sPROXY_TO_PTHREAD`; COOP/COEP needed (`tools/web/serve.py`) | `gta4-recomp/CMakeLists.txt` |
| Apple-only bridges, community MP | Report unavailable / not built on web | `gta4-recomp/src/web/web_platform_bridges.cpp` |

## Rebuilding (cloud session gotchas)

```bash
# emsdk (not in the repo)
git clone --depth 1 https://github.com/emscripten-core/emsdk.git <scratch>/emsdk
<scratch>/emsdk/emsdk install latest && <scratch>/emsdk/emsdk activate latest
source <scratch>/emsdk/emsdk_env.sh
pip install "cmake>=3.29"            # system CMake was 3.28

# SDK submodules only (shallow), then the repo's reviewed patches
P=glue/rexglue-sdk-main/thirdparty
git submodule update --init --depth 1 $P/{cli11,libmspack,FFmpeg,tomlplusplus,simde,xxHash,spdlog,fmt,snappy,utfcpp,imgui,sdl3,o1heap,inja}
python3 tools/setup_repo.py --prepare-only   # patches FFmpeg + libmspack

cmake -S glue/rexglue-sdk-main -B out/web -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/web-emscripten.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DREXGLUE_BUILD_GTA4_RECOMP=ON
ninja -C out/web LibertyRecomp rex-web-memory-test
```

- **zlib port:** the proxy blocks GitHub archive downloads. Clone
  `madler/zlib` at `v1.3.2`, unpack it into `$(em-config CACHE)/ports/zlib/zlib-1.3.2`,
  and write the port URL to `ports/zlib/.emscripten_url`.
- **Node:** use emsdk's Node 24; the system Node 22 lacks Memory64. (On the
  user's Mac, Homebrew Node 26 also works.)
- **User's Mac:** emsdk is at `~/emsdk`; the Node test build is `out/web-node`
  (`LIBERTY_WEB_NODERAWFS=ON`) and the browser build `out/web`; the game is
  installed in `~/.local/share/LibertyRecomp/game`. Dawn's npm package lives in a
  session scratchpad (`.../4f80a7c2-.../scratchpad/npm`); reinstall with
  `npm install webgpu ws` if it is gone. A relink of `out/web-node` takes ~1.5 min.
- **Timing:** a full rebuild is about 7 minutes (generated code); a relink alone is about 1.5–6 minutes.
- **Submodule status:** the patched FFmpeg/libmspack submodules show as
  modified. That is expected; never commit them. This clone sets
  `submodule.<path>.ignore=dirty` locally.
- **Build options:** `LIBERTY_WEB_NODERAWFS=ON` gives a Node-only test build (host filesystem,
  offscreen SDL, forwards `XDG_DATA_HOME`, injects Dawn when `LIBERTY_DAWN_NODE` is set).
  Turn it off for the browser. `LIBERTY_WEB_FUNCTION_NAMES=ON` keeps wasm function names
  for readable traps.
- **emdawnwebgpu port:** downloaded from Dawn's GitHub releases on first use (this worked
  through the proxy, unlike the zlib archive).
- **WebGPU under Node:** `npm install webgpu ws` (Dawn's bindings; `ws` because the
  game opens a socket). There is no GPU here: point `VK_ICD_FILENAMES` at
  `/opt/pw-browsers/chromium-*/chrome-linux/vk_swiftshader_icd.json`.
- **Shader tools** (only to regenerate the archive): `naga-cli` via `cargo install`,
  `spirv-tools spirv-cross glslang-tools` via apt, `pip install zstandard`. The
  `tools/XenosRecomp` submodule provides SMOL-V. See WEB_BUILDING.md › Shaders.
- **Logs:** nothing at info level is printed without `--diagnostics=true`.

## Testing with game files

The user supplied their own disc ISO and TU#8 package for local testing only.
They live in a session scratchpad and were **never** committed or uploaded. Do
not put game files in the repo. So far the container has kept them (with emsdk)
between sessions under
`/tmp/claude-0/-home-user-LibertyRecomp-web-test/3bed923f-3d76-5082-9c2b-35e41823c0fc/scratchpad/`
(`gamefiles/data` is an installed `XDG_DATA_HOME`, `emsdk/` the SDK); check
before relying on it, and ask the user again if they are gone.

```bash
XDG_DATA_HOME=<data> node out/web/LibertyRecomp/LibertyRecomp.js --diagnostics=true \
  --install_source=<extracted disc dir> --install_update_source=<TU_... package>
# later runs: drop the install options; add --gta4_log_guest_debug_print=true to see the
# title's own messages (most stub calls are non-strings and print as noise)

# with the renderer (see "WebGPU under Node" above):
NODE_PATH=<npm>/node_modules LIBERTY_DAWN_NODE=<npm>/node_modules/webgpu \
VK_ICD_FILENAMES=/opt/pw-browsers/chromium-1194/chrome-linux/vk_swiftshader_icd.json \
XDG_DATA_HOME=<data> node out/web/LibertyRecomp/LibertyRecomp.js --diagnostics=true \
  --webgpu_frame_dump_path=<dir>/f --webgpu_frame_dump_interval=120
# frames are PAM images (header + raw RGBA; convert with a few lines of Python
# to view them); --webgpu_trace_frame=N logs every command of frame N (with
# --log_level=info): decoded vertex inputs, nonzero pixel constants, bound
# textures, resolves and render-phase markers, and dumps that frame too.
# N and dump names are the title's submitted frame numbers.
# Gameplay starts after roughly 2500-4200 presented frames on SwiftShader. With
# Dawn on Metal and 60 fps pacing the intro starts around frame 5000-6000.
```

On the user's Mac (Dawn on Metal; no `VK_ICD_FILENAMES`), with the scratchpad
Dawn package named under Rebuilding:

```bash
N=<scratchpad>/npm/node_modules
NODE_PATH=$N LIBERTY_DAWN_NODE=$N/webgpu XDG_DATA_HOME=$HOME/.local/share \
node out/web-node/LibertyRecomp/LibertyRecomp.js --diagnostics=true \
  --diagnostics_categories=logging --log_level=info \
  --webgpu_frame_dump_path=<dir>/f --webgpu_frame_dump_interval=120 \
  --webgpu_trace_frame=6000 --webgpu_trace_pixel=565,150
```

- The outdoor intro (ship's deck, skyline, lamps) is around frames 5500-7400,
  the indoor cutscenes from about 7600. Frame numbers drift by a shot or so
  between runs, so compare runs by scene, not only by number.
- `--webgpu_trace_pixel=X,Y` (with a traced frame) logs `webgpu-pixel:` lines:
  each draw that changed that texel of its first color target, old -> new raw
  texel bytes (the back buffer is RGBA16F: four little-endian halves).
- `--webgpu_skip_pixel_shader=HASH,...` drops draws by pixel shader hash, to
  see what an effect contributes (compare dumps with and without).
- The draw trace logs each draw's stencil state
  (`stencil=enable/func/ref/mask/writemask ops=fail,depthfail,pass`); resolves
  log the requested/owner MSAA sample types, and handoffs their policy.
- Shader hashes map to WGSL in the archive; the archive format is
  `LRWGSL02` (zlib) with per-record hash, stage, variant and code, which a short
  Python script can unpack to read a shader.

## Known gaps

- Renderer gaps (all handled by the Metal renderer, which is the reference):
  MSAA, temporal AA/upscaling, SMAA/FXAA, modern post-processing (DoF, sun
  shafts, FusionFix tone LUT), vector fonts, virtual/reflection targets at a
  different physical size, separate color/alpha blend constants, border colors,
  wireframe, readback of 3D/compressed GPU textures.
- Write watches (page-protection faults) never fire; the renderer trusts the
  title's `ResourceUnlock` notifications plus a byte compare when a capture is
  marked dirty. A buffer the title writes without unlocking would go stale.
- Performance: identical uniform blocks share a slot within a batch, redundant
  pass state is skipped, texture bind groups and samplers are cached across
  frames, and the hot byte compares and swaps use SIMD128 (Emscripten's libc
  does them per byte). Measured in Chrome: ~13 µs of render-thread time per
  draw in an earlier gameplay run (uniforms ~2, encoding ~3, the rest per-draw
  setup), but ~20–32 µs in the 2026-10-07 intro run, rising with draw count
  (6,600 draws: inputs 36, bindings 25, uniforms 29, encode 39 ms a frame).
  The game thread spends ~30–35% of its time capturing. Still open: device-block
  dirty deltas instead of 22 KB copies, fewer per-draw allocations, and
  asynchronous pipeline compiles (native Dawn compiles synchronously, about
  140 ms per pipeline on Metal).
- Chrome's audio fails: SDL3's audio callback throws `Cannot mix BigInt and
  other types` in `CPtrToHeap32Index` (a wasm64 bug in SDL's JavaScript).
- The shader archive is zlib (12 MB); zstd would be 4.7 MB but needs a wasm zstd.
- The `0x90000000` view does not mirror `0x80000000` on the web.
- Guest FP rounding and flush modes are recorded but not applied.
- Guest DNS is reported as host-not-found and the local IP as loopback; there is
  no online play.
- Thread suspend and APC wakes rely on `pthread_kill`, which Emscripten runs
  only when the target calls `nanosleep` or returns to its event loop. GTA IV
  does not suspend threads, and alertable waits poll their APC queue, so
  nothing depends on it today.
- The `Too few processor cores` warning appears with 4 Node workers; it is harmless.

## Next steps (pick with the user)

1. **WebGPU renderer performance.** The renderer is done as a title-command
   renderer (the same interface the desktop gta4-native and gta4-metal
   renderers use, not Xenos emulation), and Chrome renders the intro correctly
   (see above). In Chrome, pipeline compiles and texture decoding are already
   near zero per frame; the cost is per-draw setup (inputs, bindings,
   uniforms, encoding) and the game thread's captures. Next: cut per-draw work
   and allocations, send device-block dirty deltas, then confirm Chrome
   through gameplay proper (11–15k draws). Later: compile pipelines off the
   render thread and close the fidelity gaps above.
2. **In-browser game files.** A file or folder picker (File System Access API /
   OPFS), mounted so the existing `gta4::install::Install()` can read it;
   reuse the non-interactive install path in `GTA4App::OnFinalizePaths`. Files
   are 7–8 GB, so stream them rather than preloading into MEMFS. (Local runs
   already stream an installed game from `serve.py`; the lazy mount in
   `res/web/index.html` is a model for this.)
3. **Audio, input and page lifecycle** (SDL audio context unlock, pointer lock, fullscreen).
