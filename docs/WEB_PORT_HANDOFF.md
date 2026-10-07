# Web Port Handoff

Status of the WebAssembly port on branch `claude/zen-goodall-jpl0op`.
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
- In headless Chromium the browser build still starts and stops at the install
  check (no game files). The canvas hand-off to the render worker and surface
  setup were verified with a standalone prototype; WebGPU canvas contents cannot
  be screenshotted in this container, so the browser picture is unverified.
- `rex-web-memory-test` (26 checks: alias folding, byte order, heaps, MMIO,
  returned-pointer aliases) passes under Node 24.
- Next step candidates: **in-browser game-file loading** (needed to see the
  renderer in a real browser), or renderer fidelity/performance. Ask the user.

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
| GPU threading | One render pthread owns the device and runs from the JS event loop (needed for `mapAsync` and canvas presentation), woken via `emscripten_proxy_async`. Game threads capture device block/buffers/textures at submit (`Capture` in `graphics_system.cpp`) | `gta4_webgpu/graphics_system.cpp`, `work.h` |
| Shaders | Stock SPIR-V → GLSL (SPIRV-Cross) → rewrite BDA constants to one UBO (VS 0, PS 4096, shared 8192, spec word 8192+0x500) and bindless to fixed slots → glslang → spirv-opt → naga → WGSL. naga undoes the Vulkan y-flip itself | `tools/webgpu/spirv_to_wgsl.py`, `LibertyRecompLib/shader/webgpu_shader_archive.bin` |
| Vertex data | Every attribute decoded to `float32x4` on the CPU per buffer generation (shaders read vec4 floats; WebGPU cannot feed integer formats to them) | `gta4_webgpu/vertex_decode.h`, `resources.cpp` |
| Render targets | Single-sampled; resolved depth stored as `r32float`; resolves pick the latest surface at the same EDRAM placement | `gta4_webgpu/resources.cpp`, `passes.cpp` |
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
- **Node:** use emsdk's Node 24; the system Node 22 lacks Memory64.
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
# to view them); --webgpu_trace_frame=N logs every command of frame N.
# Gameplay starts after roughly 2500-4200 presented frames on SwiftShader.
```

## Known gaps

- Renderer gaps (all handled by the Metal renderer, which is the reference):
  MSAA, temporal AA/upscaling, SMAA/FXAA, modern post-processing (DoF, sun
  shafts, FusionFix tone LUT), vector fonts, virtual/reflection targets at a
  different physical size, the forward-depth handoff's stencil rebuild (WebGPU
  cannot export stencil), separate color/alpha blend constants, border colors,
  wireframe, readback of 3D/compressed GPU textures.
- Write watches (page-protection faults) never fire; the renderer trusts the
  title's `ResourceUnlock` notifications plus a byte compare when a capture is
  marked dirty. A buffer the title writes without unlocking would go stale.
- Performance is untuned: each draw compares/copies the 22 KB device block
  (the Vulkan renderer sends dirty deltas instead), textures and vertex
  conversions run on the render thread, and pipelines compile synchronously.
- The shader archive is zlib (12 MB); zstd would be 4.7 MB but needs a wasm zstd.
- The `0x90000000` view does not mirror `0x80000000` on the web.
- Guest FP rounding and flush modes are recorded but not applied.
- Guest DNS is reported as host-not-found and the local IP as loopback; there is
  no online play.
- Thread suspend and APC wakes rely on `pthread_kill`, which is delivered when
  the target worker services its mailbox.
- The `Too few processor cores` warning appears with 4 Node workers; it is harmless.

## Next steps (pick with the user)

1. **In-browser game files.** A file or folder picker (File System Access API /
   OPFS), mounted so the existing `gta4::install::Install()` can read it;
   reuse the non-interactive install path in `GTA4App::OnFinalizePaths`. Files
   are 7–8 GB, so stream them rather than preloading into MEMFS.
2. **WebGPU renderer.** Done as a title-command renderer (the same interface
   the desktop gta4-native and gta4-metal renderers use, not Xenos emulation).
   Follow-ups: profile a gameplay frame first (split SwiftShader time vs
   pipeline compiles vs texture decode), then compile pipelines and decode
   textures off the render thread, send device-block dirty deltas, close the
   fidelity gaps above, and check the picture in a real browser once files load.
3. **Audio, input and page lifecycle** (SDL audio context unlock, pointer lock, fullscreen).
