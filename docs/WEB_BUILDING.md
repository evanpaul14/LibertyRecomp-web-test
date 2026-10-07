# Web / WebAssembly Build (Experimental)

> [!WARNING]
> The web port is at an early stage. It builds, links and starts in a browser and
> renders with **WebGPU**, but the browser cannot load your game files yet, so the
> game itself has only been run under Node.js so far.
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
- A browser with WebAssembly Memory64, threads and WebGPU: Chrome/Edge 133+.
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
`LibertyRecomp.wasm` (about 116 MB, including 12 MB of translated shaders) and
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
# spirv-cross, glslangValidator and spirv-opt from your distribution or the Vulkan SDK
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
fall-through, and then runs `spirv-opt` and naga. All 2062 shader variants
translate and pass Tint's validation in Dawn.

## Run

WebAssembly threads need `SharedArrayBuffer`, which browsers only expose to
**cross-origin isolated** pages. Serve the output with the included server, which
adds the `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers:

```bash
python3 tools/web/serve.py            # serves out/web/LibertyRecomp on :8080
```

Then open <http://localhost:8080/>. Command-line options can be passed as
repeated `arg` query parameters, for example `?arg=--diagnostics=true`.

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
Without a hardware Vulkan driver, Chromium's bundled SwiftShader works as the
device. The game also needs the `ws` package once it opens a socket:

```bash
npm install --prefix /tmp/dawn webgpu ws
NODE_PATH=/tmp/dawn/node_modules LIBERTY_DAWN_NODE=/tmp/dawn/node_modules/webgpu \
VK_ICD_FILENAMES=/path/to/chromium/vk_swiftshader_icd.json \
XDG_DATA_HOME=/path/to/data node out/web/LibertyRecomp/LibertyRecomp.js --diagnostics=true \
    --webgpu_frame_dump_path=/tmp/frames/f --webgpu_frame_dump_interval=60
```

`--webgpu_frame_dump_path` writes every Nth presented frame as a PAM image (RGBA).
`--webgpu_trace_frame=N` logs every title command of frame N. Every 60 frames the
renderer logs its draw, clear and resolve counts. `--gpu_plugin=none` restores the
old headless mode.

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
| Thread suspend / APC wake | Real-time signals | `pthread_kill`; delivered when the target worker services its mailbox |
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
command needs (the 22 KB device block, vertex and index buffers, texture data, UP
vertices) and queue it. Captured buffers and textures are reused until the title
reports a write (`ResourceUnlock`), so static assets are copied once.
Synchronous commands (texture locks) wait for the render thread.

**Geometry.** Title vertex shaders read every attribute as `vec4<f32>`, but WebGPU
only converts normalized formats to floats, so each stream's elements are decoded
to `float32x4` on the CPU once per buffer generation. Shader inputs are renumbered
densely (WebGPU allows 16 vertex locations; the title uses semantic locations up to
21). Fans, quads and restart strips become 32-bit index lists, as in the Metal
renderer, and UP rectangle lists get their fourth corner reconstructed.

**Targets and resolves.** Every render target is single-sampled (WebGPU only has 1×
and 4× MSAA). Color resolves copy directly or through a small conversion pass
(exponent bias, format change); resolved depth is stored as `r32float` so title
shaders can sample it with a filtering sampler. Surfaces that share an EDRAM
placement resolve from the one written last.

**Presentation.** A present renders the frontbuffer texture into the page canvas
and acknowledges the frame in the guest device block, which the title waits on.

## Status

Working:

- The full RexGlue runtime (core, system, kernel, filesystem, audio, input, UI)
  and FFmpeg compile for wasm64.
- All generated GTA IV code compiles, and the app links to a single module.
- In Chromium (tested with headless Chromium 141) the page is cross-origin
  isolated, the module instantiates, and `main()` runs on its worker. Startup
  resolves the title paths in Emscripten's virtual filesystem, applies the
  graphics policies, and reaches the installation check. Without game files it
  stops there with `GTA IV installation is not launch-ready: default.xex is
  missing or unreadable.`
- The canvas hand-off to the render thread works in Chromium: the canvas arrives
  in the render worker at the page size, and surface configuration and rendering
  run without WebGPU errors. Headless Chromium in a container without a GPU does
  not show WebGPU canvas contents in screenshots, so the browser's picture has not
  been checked by eye yet.

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
  reach and runs at well under one frame per second.

`--gta4_log_guest_debug_print=true` logs the title's own debug messages (the
retail build discards them), which is the quickest way to see why it stops.

Log lines need `?arg=--diagnostics=true`, the same as on desktop. On the web
they go to the browser console and the page's log panel.

Not working yet:

- **Renderer gaps** (all present in the Metal renderer): MSAA, temporal AA and
  upscaling, SMAA/FXAA, the modern post-processing effects (depth of field, sun
  shafts, FusionFix tone mapping), vector font replacement, virtual and reflection
  targets at a different physical resolution, the stencil rebuild of the
  forward-pass depth handoff, separate color/alpha blend constants, sampler border
  colors and mirror-clamp addressing (approximated), wireframe fill, and reads of
  3D or block-compressed GPU textures.
- **Performance.** Every draw compares or copies the 22 KB device block, and
  nothing is profiled yet.
- **Write watches.** The runtime's memory-coherence tracking relies on page
  protection faults, which wasm does not have. The renderer instead relies on the
  title's own unlock notifications.
- **`0x90000000` mirror.** Natively this view mirrors `0x80000000`. On the web
  it has its own backing; nothing is known to rely on the mirror.
- **Game files.** There is no way yet to give the browser build your game files.

## Roadmap

1. **Memory and MMIO.** Done. The web memory macros live in the generated
   header (`gta4_init.h`) and its codegen template (`init_h.inja`), so the
   per-function generated code did not need regenerating.
2. **Game files.** Load the user's files through the File System Access API or
   OPFS, and adapt the installer to the browser.
3. **WebGPU renderer.** Done for the title-command path; see the gaps above.
4. **Audio, input and networking.** Hook SDL audio and input up to the page
   lifecycle. Replace the UDP and HTTP online backends with WebRTC and `fetch`.

Expect the web build to run noticeably slower than native even once these are done.
