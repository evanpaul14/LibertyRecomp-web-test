# Web / WebAssembly Build (Experimental)

> [!WARNING]
> The web port is at an early stage. It builds, links and starts in a browser,
> but it runs **headless** (no rendering) and cannot get far into the game yet.
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
- A browser with WebAssembly Memory64 and threads: Chrome/Edge 133+ or Firefox 134+.
  Safari is not supported yet.
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
`LibertyRecomp.wasm` (about 64 MB) and `index.html`.

The 89 generated translation units take a few minutes to compile. The final link
also takes several minutes, because Emscripten optimizes the whole module.

The build configures the SDK directly, not through the top-level `CMakeLists.txt`,
because the web target is built from the RexGlue consumer in
`glue/rexglue-sdk-main/gta4-recomp`.

On the first build, Emscripten downloads its zlib port from GitHub. On a network
that blocks GitHub archive downloads, put a local copy in the port cache
(`$(em-config CACHE)/ports/zlib/`) first.

## Run

WebAssembly threads need `SharedArrayBuffer`, which browsers only expose to
**cross-origin isolated** pages. Serve the output with the included server, which
adds the `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers:

```bash
python3 tools/web/serve.py            # serves out/web/LibertyRecomp on :8080
```

Then open <http://localhost:8080/>. Command-line options can be passed as
repeated `arg` query parameters, for example `?arg=--diagnostics=true`.

Any other static host works if it sends the same headers.

## How the port works

| Area | Desktop | Web |
|------|---------|-----|
| Target | x86-64 / ARM64 | `wasm64` via `toolchains/web-emscripten.cmake` (`-sMEMORY64 -pthread -fwasm-exceptions -msimd128`) |
| Platform macro | `REX_PLATFORM_LINUX` etc. | `REX_PLATFORM_WEB` (also sets `REX_PLATFORM_LINUX` to reuse the musl/POSIX paths) and `REX_ARCH_WASM64` |
| Main loop | Process main thread | `-sPROXY_TO_PTHREAD`: `main()` runs on a worker so its blocking loop does not freeze the page |
| Guest VMX/SSE | SIMDe → SSE/NEON | SIMDe → WASM SIMD128 |
| FPSCR rounding / flush | Host MXCSR / FPCR | Per-thread virtual control word. wasm always rounds to nearest and never flushes denormals, so guest rounding modes are recorded but not applied |
| Runtime library | `librexruntime` shared library | Static library |
| GPU | `dlopen()`ed plugins (Vulkan, D3D12, Metal) | None yet; the app runs headless |
| Guest memory | 4.5 GB of file-mapping views; `0x7F`/`0xA0`/`0xC0`/`0xE0` ranges alias physical memory | One zero-filled region from `sbrk` (`memory_web.cpp`); aliased ranges are folded onto the physical copy by a 256-entry offset table (`rex/memory/web_guest_layout.h`) |
| MMIO | Page faults decoded by `MMIOHandler` | Every generated load and store checks the `0x7F000000` block and calls `MMIOHandler::CheckLoad`/`CheckStore` (`rex/system/web_guest_access.h`), falling back to memory for unregistered addresses |
| Page protection / write watches | `mprotect` + fault handler | Not available: protection calls succeed without effect, so access watches never fire |
| Fibers | ucontext / Win32 fibers | Thread fibers only (`fiber_web.cpp`); GTA IV imports no guest fiber APIs |
| FFmpeg (XMA) | Platform `config.h` | `thirdparty/ffmpeg-web/config.h`: portable C only |
| Thread suspend / APC wake | Real-time signals | `pthread_kill`; delivered when the target worker services its mailbox |
| Community multiplayer | CURL + OpenSSL backend | Not built; selecting it reports an error |
| Game Center, user music, microphone | Objective-C++ bridges | Report unavailable (`src/web/web_platform_bridges.cpp`) |
| RenderDoc | Optional | Not available |

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

`rex-web-memory-test` checks the memory layout and MMIO routing under Node 24:

```bash
ninja -C out/web rex-web-memory-test
node glue/rexglue-sdk-main/out/web-wasm64/rex-web-memory-test.js
```

Log lines need `?arg=--diagnostics=true`, the same as on desktop. On the web
they go to the browser console and the page's log panel.

Not working yet:

- **Rendering.** There is no GPU backend.
- **Write watches.** The runtime's memory-coherence tracking relies on page
  protection faults, which wasm does not have. A GPU backend will need to track
  dirty ranges another way.
- **`0x90000000` mirror.** Natively this view mirrors `0x80000000`. On the web
  it has its own backing; nothing is known to rely on the mirror.
- **Game files.** There is no way yet to give the browser build your game files.

## Roadmap

1. **Memory and MMIO.** Done. The web memory macros live in the generated
   header (`gta4_init.h`) and its codegen template (`init_h.inja`), so the
   per-function generated code did not need regenerating.
2. **Game files.** Load the user's files through the File System Access API or
   OPFS, and adapt the installer to the browser.
3. **WebGPU renderer.** Add a WebGPU backend for the Xenos command processor
   (statically linked, since there is no `dlopen`), and translate shaders from
   Xenos to SPIR-V to WGSL.
4. **Audio, input and networking.** Hook SDL audio and input up to the page
   lifecycle. Replace the UDP and HTTP online backends with WebRTC and `fetch`.

Expect the web build to run noticeably slower than native even once these are done.
