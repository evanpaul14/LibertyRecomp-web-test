# Web / WebAssembly Build (Experimental)

> [!WARNING]
> The web port is at an early stage. It builds, links and runs in a browser and
> renders with **WebGPU**, but the page cannot load your game files yet: it runs
> the game only from a local install served by `tools/web/serve.py`, or under
> Node.js.
> See [Status](#status) and [Roadmap](#roadmap).

The web build compiles the RexGlue runtime and the recompiled GTA IV code to
**64-bit WebAssembly** (`wasm64`, Memory64) with Emscripten. Guest threads run on
Web Workers using `SharedArrayBuffer`.

As with every other platform, no game files are included. The browser build will
need the user's own files (see [DUMPING-en.md](DUMPING-en.md)).

## Requirements

- Emscripten SDK **4.0 or newer** (Memory64 support). Tested with emsdk 6.0.11.
- CMake 3.25+, Ninja, Python 3.10+.
- Dependency sources prepared as for desktop builds (`python3 tools/setup_repo.py`).
  The web build needs only the RexGlue SDK submodules; GPU, desktop-tool and
  console submodules are not used.
- A browser with WebAssembly Memory64, threads, relaxed SIMD and WebGPU: Chrome/Edge 133+.
  The renderer requires the WebGPU features `clip-distances` and
  `float32-filterable`, and uses `texture-compression-bc` for the game's
  compressed textures (desktop GPUs). Firefox and Safari are not supported yet.
- To run under Node.js: **Node 24+** (Memory64). emsdk ships a suitable Node.

## Build

```bash
source <emsdk>/emsdk_env.sh

cmake -S glue/rexglue-sdk-main -B out/web -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/web-emscripten.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -DREXGLUE_BUILD_GTA4_RECOMP=ON

ninja -C out/web LibertyRecomp
```

Output goes to `out/web/LibertyRecomp/`: `LibertyRecomp.js` (the loader),
`LibertyRecomp.wasm` (about 112 MB, including 11 MB of translated shaders) and
`index.html`.

The 89 generated translation units take a few minutes to compile. The final link
also takes several minutes, because Emscripten optimizes the whole module.

The build configures the SDK directly, not through the top-level `CMakeLists.txt`,
because the web target is built from the RexGlue consumer in
`glue/rexglue-sdk-main/gta4-recomp`.

On the first build, Emscripten downloads its zlib port and the `emdawnwebgpu` port
(Dawn's `webgpu.h` for the browser) from GitHub. On a network that blocks GitHub
archive downloads, put a local copy in the port cache (`$(em-config CACHE)/ports/`)
first.

### Shaders

The renderer uses the game's shaders from the same stock shader cache as the
desktop renderers (`LibertyRecompLib/shader/shader_cache.cpp`), translated
offline to WGSL and stored in `LibertyRecompLib/shader/webgpu_shader_archive.bin`,
which is checked in and embedded in the module. Regenerate it only after the shader
cache changes:

```bash
pip install zstandard                       # reads the cache's compressed SPIR-V
cargo install naga-cli                      # SPIR-V → WGSL
# spirv-cross, glslangValidator and spirv-opt from your distribution, Homebrew or the
# Vulkan SDK. Last built with naga-cli 30.0.1, SPIRV-Cross 2026-09-16 (Homebrew),
# glslang 16.3.0 and SPIRV-Tools v2026.2; the GLSL rewrite also accepts the older
# SPIRV-Cross output the first archive was built from.
g++ -O2 -o /tmp/decode_smolv tools/decode_smolv.cpp \
    tools/XenosRecomp/thirdparty/smol-v/source/smolv.cpp -Itools/XenosRecomp/thirdparty/smol-v/source
python3 tools/webgpu/extract_shader_cache.py --cache LibertyRecompLib/shader/shader_cache.cpp \
    --decoder /tmp/decode_smolv --output /tmp/spv
python3 tools/webgpu/spirv_to_wgsl.py --spirv-dir /tmp/spv --work /tmp/wgsl \
    --output LibertyRecompLib/shader/webgpu_shader_archive.bin
```

The stock SPIR-V reaches its constants through buffer device addresses and its
textures through bindless arrays, neither of which WebGPU has. The translator
merges the three constant blocks into one uniform buffer (vertex constants at byte
0, pixel constants at 4096, shared constants at 8192), resolves each bindless index
to a fixed texture slot (`@group(1) @binding(slot)`, its sampler at `32 + slot`),
turns the pipeline specialization constant into a uniform word, removes switch
fall-through, and then runs `spirv-opt` and naga. Finally `split_uniforms.py`
splits the uniform buffer into three parts (vertex constants, pixel constants,
shared constants), so the renderer can reuse each one while it is unchanged,
and `draw_constants.py` makes the shaders read all three from one read-only
storage buffer at register indices taken from a per-draw record (see
Constants below). All 2062 shader variants translate and pass Tint's
validation in Dawn.

## Run

WebAssembly threads need `SharedArrayBuffer`, which browsers only expose to
**cross-origin isolated** pages. Serve the output with the included server, which
adds the `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers:

```bash
python3 tools/web/serve.py            # serves out/web/LibertyRecomp on :8080
```

Then open <http://localhost:8080/>. Command-line options can be passed as
repeated `arg` query parameters, for example `?arg=--diagnostics=true`.

For local testing, the server also exposes an installed game directory
(`--game`, default `$XDG_DATA_HOME/LibertyRecomp/game`, the same contents as a
desktop install) at `/game/` with a manifest and HTTP range requests. The page
mounts those files lazily into Emscripten's filesystem, so the archives are read
in chunks rather than copied into memory. Install the game first (for example
with the Node build below); the installer cannot map a 7.8 GB `.iso` under Node,
so give it an extracted disc folder.

The page has two stacked canvases: WebGPU draws into `#liberty-gpu`, which the
renderer's thread receives as an `OffscreenCanvas`, and SDL keeps `#canvas` on top
for input.

Any other static host works if it sends the same headers.

### Headless testing under Node.js

For testing without a browser, configure with `-DLIBERTY_WEB_NODERAWFS=ON`.
That build reads the host filesystem directly, picks up `XDG_DATA_HOME`/`HOME`
from the environment, and uses SDL's offscreen video driver:

```bash
cmake out/web -DLIBERTY_WEB_NODERAWFS=ON && ninja -C out/web LibertyRecomp
XDG_DATA_HOME=/path/to/data node out/web/LibertyRecomp/LibertyRecomp.js --diagnostics=true
```

The game is then read from `$XDG_DATA_HOME/LibertyRecomp/game`, which needs the
same contents as a desktop install: the USA retail 1.00 disc files plus the v8
title update's `default.xexp`. The build only runs under Node, so switch the
option back off for the browser.

Node has no WebGPU of its own. Dawn's Node bindings (the npm package `webgpu`)
provide it; point `LIBERTY_DAWN_NODE` at the package and every worker gets a GPU.
On macOS Dawn uses Metal directly. On Linux without a hardware Vulkan driver,
Chromium's bundled SwiftShader works as the device (set `VK_ICD_FILENAMES` as
below). The game also needs the `ws` package once it opens a socket:

```bash
npm install --prefix /tmp/dawn webgpu ws
NODE_PATH=/tmp/dawn/node_modules LIBERTY_DAWN_NODE=/tmp/dawn/node_modules/webgpu \
VK_ICD_FILENAMES=/path/to/chromium/vk_swiftshader_icd.json \
XDG_DATA_HOME=/path/to/data node out/web/LibertyRecomp/LibertyRecomp.js --diagnostics=true \
    --webgpu_frame_dump_path=/tmp/frames/f --webgpu_frame_dump_interval=60
```

`--webgpu_frame_dump_path` writes every Nth presented frame as a PAM image (RGBA),
named by the title's frame number. `--webgpu_trace_frame=N` logs every title
command of that frame (with each draw's decoded vertex inputs, nonzero pixel
constants and bound textures) and always dumps it; trace lines are info-level, so
add `--log_level=info`. Every 60 frames the renderer logs its draw, clear and
resolve counts. `--webgpu_perf_report=true` times the renderer's stages and logs
every 5 s as warnings: fps, draws and render passes a frame, render-thread
time per stage, WebGPU calls per draw, the longest frame and GPU frame latency.
Without it no stage timing runs (each clock read is a call out to JavaScript).
`--webgpu_frame_limit` caps presents per second (default 60, 0 = unlimited).
Title pipelines are created asynchronously, and a draw is skipped until its
pipeline is ready (`--webgpu_async_pipelines=false` creates them synchronously;
a traced frame always does). Shader modules for registered shaders are created
ahead of their first draw, `--webgpu_shader_warmup_ms` (default 4) of
render-thread time a frame, mostly during the first loading screens.
With a traced frame, `--webgpu_trace_pixel=X,Y` logs every draw that changed
that texel of its first color target, and `--webgpu_skip_pixel_shader=HASH,...`
drops draws by pixel shader, to see what an effect contributes.
`--gpu_plugin=none` restores the old headless mode. If no frame is presented
for 15 seconds, the renderer logs every thread and what it is waiting on.

## How the port works

| Area | Desktop | Web |
|------|---------|-----|
| Target | x86-64 / ARM64 | `wasm64` via `toolchains/web-emscripten.cmake` (`-sMEMORY64 -pthread -fwasm-exceptions -msimd128`) |
| Platform macro | `REX_PLATFORM_LINUX` etc. | `REX_PLATFORM_WEB` (also sets `REX_PLATFORM_LINUX` to reuse the musl/POSIX paths) and `REX_ARCH_WASM64` |
| Main loop | Process main thread | `-sPROXY_TO_PTHREAD`: `main()` runs on a worker so its blocking loop does not freeze the page |
| Guest VMX/SSE | SIMDe → SSE/NEON | SIMDe → WASM SIMD128 |
| FPSCR rounding / flush | Host MXCSR / FPCR | Per-thread virtual control word. wasm always rounds to nearest and never flushes denormals, so guest rounding modes are recorded but not applied |
| Runtime library | `librexruntime` shared library | Static library |
| GPU | `dlopen()`ed plugins (Vulkan, D3D12, Metal) | WebGPU title renderer, statically linked (`src/graphics/gta4_webgpu`); see [WebGPU renderer](#webgpu-renderer) |
| Guest memory | 4.5 GB of file-mapping views; `0x7F`/`0xA0`/`0xC0`/`0xE0` ranges alias physical memory | One zero-filled region from `sbrk` (`memory_web.cpp`); aliased ranges are folded onto the physical copy by a 256-entry offset table (`rex/memory/web_guest_layout.h`) |
| MMIO | Page faults decoded by `MMIOHandler` | Every generated load and store checks the `0x7F000000` block and calls `MMIOHandler::CheckLoad`/`CheckStore` (`rex/system/web_guest_access.h`), falling back to memory for unregistered addresses |
| Page protection / write watches | `mprotect` + fault handler | Not available: protection calls succeed without effect, so access watches never fire |
| Fibers | ucontext / Win32 fibers | Thread fibers only (`fiber_web.cpp`); GTA IV imports no guest fiber APIs |
| FFmpeg (XMA) | Platform `config.h` | `thirdparty/ffmpeg-web/config.h`: portable C only |
| Fused multiply-add (`std::fma`) | Hardware instruction | wasm has none, so libc's software `fma` is replaced by relaxed SIMD's `f64x2.relaxed_madd` (`src/web/web_fma.cpp`) |
| Thread suspend / APC wake | Real-time signals | `pthread_kill`; delivered when the target worker services its mailbox |
| Host clock | `CLOCK_MONOTONIC_RAW` (Linux), `mach_absolute_time` (macOS) | `CLOCK_MONOTONIC`: Emscripten has no raw clock (`src/core/clock_posix.cpp`) |
| Community multiplayer | CURL + OpenSSL backend | Not built; selecting it reports an error |
| Game Center, user music, microphone | Objective-C++ bridges | Report unavailable (`src/web/web_platform_bridges.cpp`) |
| RenderDoc | Optional | Not available |

## WebGPU renderer

Like the desktop `gta4-native` (Vulkan) and `gta4-metal` renderers, the web
renderer consumes GTA IV's **title commands** (`rex/graphics/gta4_native/title_commands.h`),
the D3D-level stream the game's device hooks produce, rather than emulating the
Xenos command processor. Its behavior follows the Metal renderer.

**Threads.** WebGPU objects belong to the JavaScript worker that created them,
and buffer mapping and canvas presentation only happen when that worker returns to
its event loop. One render thread therefore owns the device and runs from the event
loop; it is woken through Emscripten's proxying queue. The game's threads, which
expect guest memory to be read when a command is submitted, capture what each
command needs (the parts of the 22 KB device block that changed since the last
draw, vertex and index buffers, texture data, UP vertices) and queue it; the
render thread keeps its own copy of each device block. Captured buffers and textures are reused until the title
reports a write (`ResourceUnlock`), so static assets are copied once.
Synchronous commands (texture locks) wait for the render thread.

**Geometry.** Title vertex shaders read every attribute as `vec4<f32>`, but WebGPU
only converts normalized formats to floats, so each stream's elements are decoded
to `float32x4` on the CPU once per buffer generation. Shader inputs are renumbered
densely (WebGPU allows 16 vertex locations; the title uses semantic locations up to
21). Fans, quads and restart strips become 32-bit index lists, as in the Metal
renderer, and UP rectangle lists get their fourth corner reconstructed.
Converted vertex and index data share large pooled buffers; a draw selects its
data with `firstIndex` and `baseVertex`, so consecutive draws keep one binding
(every WebGPU call crosses from wasm into the browser).

**Constants.** The title's vertex constants, pixel constants and shared
constants live in slots of one per-batch arena, which shaders read as a single
read-only storage buffer (bound once per render pass). A draw writes a new
slot only for a part whose inputs changed, judged by which chunks of the
device block changed, then a 16-byte draw record with the three slots'
register indices, and passes the record's index as `firstInstance`. The
vertex shader reads it through `instance_index` and passes it to the pixel
shader in a flat varying (location 18), so selecting constants costs no
WebGPU call. This needs 20 inter-stage variables; the renderer refuses an
adapter with fewer.

**Targets and resolves.** Every render target is single-sampled (WebGPU only has 1×
and 4× MSAA). Color resolves copy directly or through a small conversion pass
(exponent bias, format change); resolved depth is stored as `rg32float` (depth and stencil) so title
shaders can sample it with a filtering sampler. Surfaces that share an EDRAM
placement resolve from the one written last. When that surface has a different
MSAA layout than the resolved view (the title downsamples its bloom and exposure
chain by resolving a 4× view of a 1× surface), the selected samples are mapped
onto it and averaged, as the native renderer's conversion shader does.

**Depth handoff.** After the G-buffer, the title moves scene depth to the forward
depth surface and asks for the stencil to be rebuilt: 0x80 where the scene is
empty, 0xFF where it is covered. The deferred lighting passes test that stencil.
The renderer copies depth from the resolved snapshot and writes the stencil with
a fixed-function stencil Replace.

**Presentation.** A present renders the frontbuffer texture into the page canvas
and acknowledges the frame in the guest device block, which the title waits on.
Neither the canvas nor Node blocks on vsync, so the presenting thread paces
itself to `--webgpu_frame_limit` frames a second. At most two frames are in
flight on the GPU; a present waits for an earlier one to finish.

## Status

Working:

- The full RexGlue runtime (core, system, kernel, filesystem, audio, input, UI)
  and FFmpeg compile for wasm64.
- All generated GTA IV code compiles, and the app links to a single module.
- In Chromium the page is cross-origin isolated, the module instantiates, and
  `main()` runs on its worker. Without game files it stops at the installation
  check (`GTA IV installation is not launch-ready: default.xex is missing or
  unreadable.`).
- With game files served by `tools/web/serve.py`, Chrome on an M1 Mac renders
  the loading screens and the intro cutscenes lit and correct, with no stalls
  or GPU errors. Gameplay is playable: walking and driving around Liberty City
  works. Before the per-draw work of 2026-10-07, light scenes ran at 40–59 fps,
  the heaviest cutscene stretch (about 6,000 draws a frame) at 3–6 fps and
  gameplay at about 3–10 fps. Since then it is clearly faster and gameplay
  briefly reaches playable frame rates, with stutter from shader compiles (no
  Chrome measurements recorded yet). The first run after the shader archive
  changed stalled for about a second at a time while Chrome compiled every
  pipeline; pipelines are now created asynchronously and shader modules ahead
  of use, which has not been rechecked in Chrome yet.

`rex-web-memory-test` checks the memory layout and MMIO routing under Node 24:

```bash
ninja -C out/web rex-web-memory-test
node glue/rexglue-sdk-main/out/web-wasm64/rex-web-memory-test.js
```

Under Node.js with a real USA retail 1.00 disc plus title update 8 (installed
headless with `--install_source`/`--install_update_source`), the game itself
runs into gameplay:

- the installer copies the files and applies the v8 patch (`0.0.0.5` →
  `0.0.8.5`);
- the kernel, file I/O, XMA decoder and audio threads start and the module
  launches;
- the title preloads its shaders from `common:/shaders`, allocates its
  render-target pools, initializes the audio engine and registers its script
  natives;
- with the WebGPU renderer (Dawn on SwiftShader), it draws the loading screen,
  starts a new game and renders Liberty City: the skyline at night, water
  reflections and the HUD radar, at about 8000 draws a frame with no rejected
  draws or WebGPU errors. On a CPU-emulated GPU this takes over 15 minutes to
  reach: the loading screen runs at about 24 fps, gameplay frames take 4–7
  seconds each;
- with Dawn on Metal (an M1 Mac), the loading-screen artwork renders correctly
  and the intro is reached in about two minutes at 60 fps; the heaviest intro
  frames (6,600–8,800 draws) run at about 8–12 fps (about 4.5 fps before the
  per-draw work). The intro's deferred lighting, coronas and
  bloom render correctly, and a 19-minute run reached gameplay without stalling.

`--gta4_log_guest_debug_print=true` logs the title's own debug messages (the
retail build discards them), which is the quickest way to see why it stops.

Log lines need `?arg=--diagnostics=true`, the same as on desktop. On the web
they go to the browser console and the page's log panel.

Not working yet:

- **Renderer gaps** (all present in the Metal renderer): MSAA, temporal AA and
  upscaling, SMAA/FXAA, the modern post-processing effects (depth of field, sun
  shafts, FusionFix tone mapping), vector font replacement, virtual and reflection
  targets at a different physical resolution, separate color/alpha blend
  constants, sampler border colors and mirror-clamp addressing (approximated),
  wireframe fill, and reads of 3D or block-compressed GPU textures.
- **Performance.** Heavy scenes are limited by the render thread's per-draw
  cost, about 10 µs a draw in Node with Dawn on Metal (down from about 20), of
  which about 4 µs is WebGPU calls. Pipelines compile asynchronously, so a new
  shader no longer stalls the frame; its draws are missing for the few frames
  until it is ready.
- **Write watches.** The runtime's memory-coherence tracking relies on page
  protection faults, which wasm does not have. The renderer instead relies on the
  title's own unlock notifications.
- **`0x90000000` mirror.** Natively this view mirrors `0x80000000`. On the web
  it has its own backing; nothing is known to rely on the mirror.
- **Game files.** The browser can only read game files from a local
  `serve.py`; there is no file or folder picker yet.
- **Audio in Chrome.** Audio plays. It used to crackle and run slow in busier
  scenes because the title's audio was produced at only ~75–80% of real time.
  Three web-specific costs caused that: multi-object waits polled every
  millisecond, `fma` ran in software, and the timer thread spun. They are fixed,
  and under Node the heaviest intro scenes now hold real time; this has not
  been rechecked in Chrome yet. SDL3's own pointer conversion broke on wasm64
  (every callback threw `Cannot mix BigInt and other types`);
  `res/web/sdl_wasm64.js` replaces it. After the queue runs dry, the SDL driver
  waits for `--audio_refill_frames` (12 on the web) before playing again, and
  logs `audio: … frames played, … silent (… underruns)` every 5 s while it
  underruns. `--audio_perf_report=true` logs the guest mixer's frames (938 per
  5 s is real time) and the XMA decoder's busy time every 5 s.

## Roadmap

1. **Memory and MMIO.** Done. The web memory macros live in the generated
   header (`gta4_init.h`) and its codegen template (`init_h.inja`), so the
   per-function generated code did not need regenerating.
2. **Game files.** Local runs read an installed game from `serve.py`. Still to
   do: load the user's own files through the File System Access API or OPFS, and
   adapt the installer to the browser.
3. **WebGPU renderer.** Done for the title-command path; see the gaps above.
4. **Audio, input and networking.** Hook SDL audio and input up to the page
   lifecycle. Replace the UDP and HTTP online backends with WebRTC and `fetch`.

Expect the web build to run noticeably slower than native even once these are done.
