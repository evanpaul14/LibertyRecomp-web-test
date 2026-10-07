# Web Port Handoff

Status of the WebAssembly port on branch `claude/zen-goodall-jpl0op`.
`WEB_BUILDING.md` is the user-facing guide (build, run, how each subsystem is
mapped); this file is for whoever continues the work.

## Where it stands

- The RexGlue runtime, FFmpeg and all 89 generated GTA IV translation units
  build to **wasm64** (Memory64, pthreads, wasm EH, SIMD128) and link into
  `out/web/LibertyRecomp/LibertyRecomp.{js,wasm}` (about 64 MB).
- In headless Chromium 141 the page is cross-origin isolated and the app
  starts and reaches the install check (there is no way to give it game files
  in a browser yet).
- Under Node 24 with real game files (USA retail 1.00 disc + title update 8),
  installed with the new `--install_source`/`--install_update_source`
  options, **the game runs until it needs a GPU**: XEX load and v8 patch,
  kernel/IO/audio threads, shader preload, render-target pools, audio engine
  and script-native registration. It then busy-waits for the GPU to consume its
  command ring buffer. That is the expected end of a headless build.
- `rex-web-memory-test` (26 checks: alias folding, byte order, heaps, MMIO,
  returned-pointer aliases) passes under Node 24.
- The user has not yet chosen the next step: **in-browser game-file loading**
  or the **WebGPU renderer**. Ask before starting either.

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
| GPU | None; plugins are `dlopen()`ed natively and not built on web. App runs headless on purpose | `src/graphics/CMakeLists.txt`, `gta4-recomp/src/gta4_app.cpp` |
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
  offscreen SDL, forwards `XDG_DATA_HOME`). Turn it off for the browser.
  `LIBERTY_WEB_FUNCTION_NAMES=ON` keeps wasm function names for readable traps.

## Testing with game files

The user supplied their own disc ISO and TU#8 package for local testing only.
They lived in the session scratchpad, which does not survive the session, and
were **never** committed or uploaded. Do not put game files in the repo. Ask
the user again if needed.

```bash
XDG_DATA_HOME=<data> node out/web/LibertyRecomp/LibertyRecomp.js --diagnostics=true \
  --install_source=<extracted disc dir> --install_update_source=<TU_... package>
# later runs: drop the install options; add --gta4_log_guest_debug_print=true to see the
# title's own messages (most stub calls are non-strings and print as noise)
```

## Known gaps

- No GPU backend; write watches (page-protection faults) never fire, so a
  future GPU backend needs another way to track dirty memory.
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
2. **WebGPU renderer.** A statically linked graphics system implementing the
   Xenos command processor (start from `src/graphics/command_processor.cpp` and
   the Vulkan backend in `src/graphics/vulkan/`), plus Xenos → SPIR-V → WGSL
   shader translation. Then hook it into `GTA4App::OnPreSetup` instead of
   running headless. This is what gets past the current GPU wait.
3. **Audio, input and page lifecycle** (SDL audio context unlock, pointer lock, fullscreen).
