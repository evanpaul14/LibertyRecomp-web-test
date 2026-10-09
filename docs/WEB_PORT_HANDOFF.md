# Web Port Handoff

Status of the WebAssembly port on branch `webgpu-node-graphics` (branched from
`main`; earlier work was on `claude/zen-goodall-jpl0op`).
`WEB_BUILDING.md` is the user-facing guide (build, run, how each subsystem is
mapped); this file is for whoever continues the work.

## Where it stands

- The RexGlue runtime, FFmpeg and all 89 generated GTA IV translation units
  build to **wasm64** (Memory64, pthreads, wasm EH, SIMD128) and link into
  `out/web/LibertyRecomp/LibertyRecomp.{js,wasm}` (about 112 MB, 11 MB of it the
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
  - *Rechecked with those fixes (before the per-draw work below):* the intro cutscenes render lit and correct
    (ship's cabin, the docks meeting with the in-world credits) with no stalls
    or GPU errors. Light scenes (400–700 draws) run at 40–59 fps; the heaviest
    stretch (~6,000–6,600 draws) dropped to 3–6 fps, at ~20–32 µs of
    render-thread time per draw, spread over inputs, bindings, uniforms and
    encoding (pipeline compiles were near zero). Gameplay is confirmed: the
    user played and drove around at about 3–10 fps.
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
- **Profiling.** `--webgpu_perf_report=true` turns on stage timing (off
  otherwise: each clock read is a call to JavaScript, ~2–3 µs in a Chrome
  worker, so per-draw stages are sampled on one command in 16 and scaled;
  rare work is timed every time) and logs every 5 s as
  warnings: a `perf` line (fps, draws and render passes a frame, render-thread time per stage, new
  pipelines with how many became ready or failed, their latency and the draws
  that waited for them, shader modules warmed and still queued, new uniform
  slots, redrawn draws, WebGPU calls per draw), a stall line (the longest
  frame with the render thread's busy and pipeline time in it, GPU frame
  latency, presents that waited for the GPU), a `presents` line (presents
  shown on the canvas, those with new frontbuffer contents, those skipped: no
  source, no canvas, no surface texture with its status; canvas size,
  frontbuffer, GPU error count), a `constant chunk changes` line, the game
  thread's `capture` line and, with `--webgpu_gpu_timing=true`, GPU pass
  timing (CPU session of 2026-10-08 below). The stall line's "GPU frame
  latency" includes the time the busy render worker takes to see the
  completion; it is not GPU time. Use it
  with `--diagnostics=true
  --diagnostics_categories=logging --log_level=warn`: other diagnostics print
  a line per draw, and every line blocks the game thread until the page's
  main thread handles it. In the browser that alone held gameplay below 1 fps.
- **Per-draw performance session (2026-10-07, commits b08f89bb..e8e98c5f).**
  Render-thread cost per draw fell from ~20 to ~10 µs in Node with Dawn on
  Metal (heaviest intro stretch 4.5 → 9–11 fps; details and a benchmark table
  under Known gaps › Performance). Changes: device-block deltas, per-binding
  uniform reuse (the shader archive is now `LRWGSL03` with three uniform
  bindings), cached input layouts, pooled vertex/index buffers, retry of a
  draw interrupted by a flush, and at most two GPU frames in flight. The user
  reports Chrome is clearly faster and gameplay briefly reached playable frame
  rates, with stutter; the first run after the shader change stalled for
  about a second at a time (Chrome recompiles every pipeline when the WGSL
  changes), which fast-forwarded the opening cutscene. No Chrome perf numbers
  were recorded yet.
- **Asynchronous pipelines (2026-10-07, after e8e98c5f).** Title pipelines
  are created with `CreateRenderPipelineAsync`; a draw whose pipeline is still
  pending is skipped (`waiting` in the per-frame log, `draws a frame waited` in
  the perf line), and a failed creation is logged and its draws fail.
  `--webgpu_async_pipelines=false` restores synchronous creation; a traced
  frame always creates them synchronously, so its trace is complete. That alone
  left most of the stall: creating a *shader module* parses and validates its
  WGSL synchronously (2.4 s over the intro's first 222 pipelines, up to 144 ms
  for one module). So `kRegisterShader` now queues the shader, and after each
  present `WarmModules` creates queued modules for up to
  `--webgpu_shader_warmup_ms` (default 4; one large module can overrun it).
  Alpha-test (late) variants are queued after the rest. The title registers
  about 1,970 variants during the first loading screens, which are all done
  within ~15 s, while those screens drop to ~36 fps. In the Node intro
  benchmark the longest frame of any 5 s window fell from 490–860 ms to at most
  ~170 ms, with pipeline time in it ≤ 10 ms; few draws wait (at most 7 a frame
  in a window, while ~45 new pipelines arrive) and the frame dumps show no
  visible gaps. Per-draw cost is unchanged. Not yet checked in Chrome, where the
  same costs land in the GPU process instead of the render thread.
- **Pass merging and per-draw overhead (2026-10-07, after d7069cff).** A V8
  CPU profile of the render thread (`node --cpu-prof`; the build keeps wasm
  function names) showed the "untimed" per-draw time was mostly render passes
  and the timers themselves. The title toggles depth and color targets between
  draws, and each change ended the pass (~13 µs) and reset every binding: the
  intro ran 330–490 passes a frame. A draw now stays in the open pass when its
  attachments are among the pass's, and a new pass also attaches the other
  bound surfaces of the same size that already exist; a pipeline built for a
  pass attachment the draw does not use writes nothing to it (an empty fragment
  shader when the draw has no pixel shader). Passes fell to ~28 a frame, state
  calls from 0.24 to 0.07 a draw. Stage timers (and the game thread's capture
  timers) now run only with `--webgpu_perf_report`: each clock read is a call
  out to JavaScript (~55 ns under Node) and they took ~10–15% of `Draw`. Fan,
  quad and UP conversions reuse scratch buffers. Measured under Node with a
  busy machine (a video call running), so only roughly: ~10–15% less
  render-thread time a draw with the timers still on; frame dumps unchanged
  and no GPU errors. Not checked in Chrome yet.
  What remains a draw (Node profile): about half is calls into JavaScript;
  the largest is the uniform `SetBindGroup` (~1 µs, one a draw because vertex
  constants change on almost every draw), then the draw call (~0.9 µs). Removing
  the bind-group call would mean selecting the uniform slots with
  `firstInstance` and storage-buffer reads in every shader, plus a flat varying
  to give the pixel shader the draw index; some vertex shaders already output
  18 varyings, so that needs checking against `maxInterStageShaderVariables`.
  (Done since: see Constants by `firstInstance` below.)
- **Chrome recheck (2026-10-07, after 4ca42b25).** Intro cutscenes: 28 passes
  a frame, render thread ~6–7 µs a draw (was 20–32), no pipeline stalls (0 ms
  pipelines, no draws waiting), longest frame 1–1.5× the average. Heavy
  scenes (4,000–5,400 draws) run 13–18 fps and looked GPU-bound: every present
  waited for the GPU, GPU frame latency 40–80 ms (they were CPU-bound; see the
  CPU session of 2026-10-08). The game thread spends
  ~40% of its time capturing draws (2.1 s per 5 s). Light scenes hold 60 fps.
- **Audio in Chrome (2026-10-07).** SDL3's `CPtrToHeap32Index` divided an
  EM_ASM pointer (a Number) by `4n`, so every audio callback threw and nothing
  played. `res/web/sdl_wasm64.js` (pre-js) defines it first; SDL keeps an
  existing definition, so the submodule is untouched. Audio now plays but
  crackles and sometimes runs slow. SDL's web backend pulls 2048 samples
  (~43 ms, 8 guest frames) per callback on the page's main thread; when the
  queue is empty the driver plays silence, and the guest's audio clock only
  advances on played frames. The driver now waits for `--audio_refill_frames`
  (12 on the web, 0 elsewhere) after an underrun and logs underrun stats every
  5 s (web only). Measured: ~940 frames are needed per 5 s; light scenes kept
  up (queue 40–64 frames), but scenes of 600–760 draws played only 707–730 with
  210–234 silent (18–19 underruns, queue never above 16–18), even at 60 fps.
  So the title produces audio at ~75–80% of real time there; buffering cannot
  fix that. Likely the abrupt cutscene cuts with sped-up animation the user saw
  come from the cutscene clock following this audio clock (not verified).
- **Audio shortfall fixed in Node (2026-10-08, b8b232d1).** Node's silent
  fallback is now paced by a deadline (it slept a fixed 5.3 ms after each
  frame and hid the shortfall); with that, Node shows the same shortfall in
  the heavy intro stretch (680–760 of 938 frames per 5 s). `--audio_perf_report`
  logs the guest mixer's frames and callback time and the XMA decoder's busy
  time every 5 s (web only). A V8 CPU profile of all workers (`node --cpu-prof
  --cpu-prof-dir=<dir>`, with a `-r` preload that calls `process.exit` after N
  ms so the profiles are written; wasm futex waits count as samples, so
  subtract `_do_futex_wait`) showed:
  - XMA decoding is ~7% busy: not the cause.
  - The audio worker's guest callback spends almost all its time in
    `KeWaitForMultipleObjects`, waiting for the title's mixer thread, and
    `PosixConditionBase::WaitMultiple` polled its handles with 1 ms sleeps.
    On the web it now waits on a global epoch that every signal bumps
    (`WakeMultipleWaiters` in `threading_posix.cpp`; a futex wake only when
    someone waits, 50 ms safety cap). Desktop still polls.
  - The title's mixer thread (~70% of a core) spent about a third of its time
    in `fma`: every PowerPC `fmadd` is `std::fma`, which wasm lowers to musl's
    software fma. `gta4-recomp/src/web/web_fma.cpp` overrides the symbol with
    relaxed SIMD's `f64x2.relaxed_madd` (one hardware FMA on ARM64 and x86-64
    with FMA3; multiply then add elsewhere), so the module now needs relaxed
    SIMD (Chrome 114+). This speeds up every fmadd in the game, not only audio.
  - The timer queue's disruptorplus spin-wait busy-looped (~16% of a core;
    under Node each spin also called `os.cpus()`). On the web it uses the
    blocking strategy, whose timed waits had their arguments in the wrong
    order (never instantiated before; fixed).
  Result (Node + Dawn on Metal, same intro): the heaviest stretch now holds
  938 of 938 frames per 5 s with the mixer thread ~40% idle; renderer fps
  unchanged (13–19 fps there). Confirmed in Chrome, except while pipelines
  compile (see Chrome recheck 2026-10-08 below). To check: run with `?arg=--diagnostics=true&arg=--diagnostics_categories=logging&arg=--log_level=warn&arg=--audio_perf_report=true`,
  click the page so audio starts, and look for `audio:` underrun lines (logged
  only when underruns happen) and `audio mixer:` lines near 938 frames.
- **Constants by `firstInstance` (2026-10-08).** The per-draw uniform
  `SetBindGroup` is gone. Title shaders read their constants from one
  read-only storage buffer (the whole uniform arena, bound once per pass;
  `tools/webgpu/draw_constants.py`, archive `LRWGSL04`). Each draw writes a
  16-byte record (register indices of its vertex, pixel and shared slots,
  reused while unchanged) and passes its index as `firstInstance`; the vertex
  shader loads it with `instance_index` and hands it to the pixel shader as a
  flat `vec4<u32>` at location 18. Vertex shaders now use 20 inter-stage
  variables (18 locations, the record, a clip distance); device creation
  fails with a clear message below that (Dawn on Metal reports 31). Utility
  passes keep a 64-byte uniform binding with a dynamic offset into the same
  buffer (`utility_layout`). All 2062 variants pass Tint, and every pixel
  shader links with a vertex shader. Measured (Node + Dawn on Metal, same
  7-minute intro run, before vs after): heavy stretch (≥4,500 draws) encode
  3.0 → 2.15 µs a draw, render thread 7.55 → 6.75 µs a draw, bind-group calls
  0.80 → 0.39 a draw (the rest are texture groups); the same closing shot
  (~5,870 draws) went from 14.0–14.5 to 15.0–15.7 fps. Frame dumps look the
  same (skinned characters, credits, lighting). In Chrome, heavy-scene GPU
  frame latency is about what it was before the change (see Chrome recheck
  2026-10-08 below), so the storage reads (Apple GPUs preload uniform buffers
  but not storage reads at dynamic indices) show no clear GPU cost.
- **Shader archive regenerated from SPIR-V (2026-10-08).** The archive was
  rebuilt with the full pipeline on the user's Mac (naga-cli 30.0.1,
  SPIRV-Cross 2026-09-16 from Homebrew, glslang 16.3.0, SPIRV-Tools
  v2026.2) instead of converting the old one. Newer SPIRV-Cross
  forward-declares the pointer types and loads each bindless index into a
  temporary first; `rewrite_glsl` now accepts both forms (every variant
  failed before). All 2062 variants convert and pass Tint; record metadata
  (masks, attributes) is identical to the converted archive, and the WGSL is
  ~9% smaller (archive 12.3 → 11.4 MB). In the intro benchmark run back to
  back under the same machine load, converted vs regenerated: 15.3 vs
  15.1 fps, GPU frame latency 66.2 vs 67.6 ms in the heavy stretch (noise);
  frame dumps render correctly. Heavy scenes in Node are partly GPU-bound
  too: most presents there wait for the GPU (latency ~60–70 ms).
- **Chrome recheck (2026-10-08, build of 81d6c011: audio fixes, `firstInstance`
  constants, regenerated archive).** Chrome 154 on the M1 Mac, game served by
  `serve.py` on localhost, flags
  `--diagnostics=true --diagnostics_categories=logging --log_level=warn
  --audio_perf_report=true --webgpu_perf_report=true`, one click on the page
  ~3–10 s in to start audio. Three runs:
  1. *First run after the archive change* (the user's profile, a tab in the
     extension's tab group): the opening cutscene ran its light first ~30 s
     (300–960 draws, 22–30 passes) and then the game went straight to
     gameplay on the docks; the heavy deck shots never appeared. During those
     30 s, new pipelines took 1.4–4 s on average (max 5.7 s) with 28–90 draws
     a frame waiting; the guest mixer made only 753–867 of 938 frames per 5 s
     (9–11 underruns per 5 s) the whole time.
  2. *Reload, same profile (warm shader cache):* the whole intro played (deck,
     docks meeting with Roman, Roman's car, in-world credits; checked 3.5 min
     in). Mixer at 931–958 frames per 5 s throughout, including the
     ~8,800-draw shots; the only underruns (5–6 per 5 s) were during a 10 s
     burst of pipeline compiles (latency up to 3.4 s even with the cache warm).
  3. *Fresh Chrome profile (cold shader cache)*, driven over the DevTools
     protocol from a script (fresh `--user-data-dir`, `--remote-debugging-port`,
     console to a file, a screenshot every 10 s): compiles as slow as run 1
     (1.6–2.5 s on average, max 5.3 s) and the mixer dropped to 619–689 frames
     in three windows (10–22 underruns), yet the intro played through (the
     user watched it). So slow compiles and a mixer shortfall alone do not
     skip the intro; what did in run 1 is unknown. Not ruled out: the run-1
     tab was in the extension's tab group, and the mixer stayed low for the
     whole 25 s there instead of recovering between bursts. An idea not yet
     tried: log the cutscene prepare/stop hooks in `gta4_transition_hooks.cpp`
     (`sub_82526268`, `sub_82526058`, which today report only to the
     file-based audio handoff trace) with their callers, and repeat cold
     runs until one skips.
  Renderer (run 2; no GPU errors, every present showed new content):

  | Draws a frame | fps | GPU frame latency | Render thread per draw |
  |---|---|---|---|
  | 300–900 | 52–60 | 12–21 ms | ~11–12 µs |
  | ~4,000 | 17–19 | 55–60 ms | ~8 µs |
  | 7,600–8,800 | 10–12 | 85–100 ms | ~7.4 µs |

  Heavy scenes looked GPU-bound (nearly every present waits for the GPU);
  that was wrong, see the CPU session of 2026-10-08 below. The
  2026-10-07 Chrome recheck (before `firstInstance`) had 4,000–5,400-draw
  scenes at 13–18 fps with 40–80 ms latency, so the storage-buffer constants
  show no clear GPU cost; these runs are not an exact comparison (different
  tab and run). Gameplay in run 1 reached 10,000–11,000 draws at 6–8 fps
  with ~150 ms GPU latency. The synthetic click throws `WrongDocumentError`
  (pointer lock refused); harmless.
- **CPU session (2026-10-08, 077c40e7..695520e5).** The heavy scenes were
  never GPU-bound, and the browser clock ran guest time ~1000x fast.
  - *GPU timing:* `--webgpu_gpu_timing` (with `--webgpu_perf_report`) writes
    timestamps around every render pass (needs `timestamp-query`; Chrome
    quantizes it to 100 µs unless started with
    `--enable-webgpu-developer-features`). It logs GPU busy time a frame,
    idle time, and the passes that advanced the GPU timeline most (passes
    overlap on Apple GPUs, so each pass is charged only for how far it moved
    the end of the timeline). Heavy intro scenes (5,000–8,000 draws) keep the
    GPU busy ~18–23 ms a frame in both Node and Chrome; the costliest pass is
    the 1024x768 RGBA16F one (pixel shader 7B14CAC2A31D4199, ~50%). "GPU frame
    latency" is not GPU time: the work-done callback waits for the busy
    render worker's event loop.
  - *Chrome trace and worker profiles* (DevTools protocol; scripts were in
    the session scratchpad, easy to rewrite: `Tracing.start` on the browser
    target, and `Target.setAutoAttach` + `Profiler.start` on each worker
    session). The GPU process was ~8% busy; the render worker ~97%. Build
    the browser build with `LIBERTY_WEB_FUNCTION_NAMES=ON` for readable
    profiles. A wasm `atomic.wait` counts as busy time in V8 profiles, so
    threads blocked on condition variables look 100% busy.
  - *Fixes, in order of effect:* (1) the perf timers themselves: each clock
    read is a call to JavaScript costing ~2–3 µs in a Chrome worker, and
    per-draw timers took ~46% of the render thread; per-command stage timers
    are now sampled (1 in 16, scaled). (2) `-sMALLOC=mimalloc`: the title's
    render thread allocates each captured draw and the renderer frees it, and
    both spent ~a quarter of their time in malloc/free waiting on dlmalloc's
    lock. (3) Hot/cold constants (`tools/webgpu/hot_constants.py`, archive
    `LRWGSL05`): the title rewrites vertex and pixel registers 0–15 on almost
    every draw, so each stage now has a 16-register hot slot and a cold slot,
    and uploads fell from ~20 to ~6 MB a frame. The perf report also counts
    constant chunk changes. Chrome, heavy intro scenes: ~6,000–7,300 draws a
    frame at ~24–27 fps (was ~10–14 at fewer draws).
  - *Clock:* `host_tick_frequency_platform()` used `clock_getres`, which
    browsers report as 1000 ns while the count is nanoseconds, so guest time
    ran ~1000x fast in Chrome (Node reports 1 ns). The title clamps its frame
    step, so the game ran at a fixed fast-forward that grew with the frame
    rate; it went unnoticed at 3–10 fps. Fixed (1 GHz on Emscripten); the
    user confirmed normal speed when driving.
  - *Audio:* faster rendering made audio stutter: each XMA kick woke the
    decoder thread and waited for it (two thread wakes, ~1,100 kicks a second),
    and in busy scenes those delays held the guest mixer below real time
    while the decoder was ~80% idle. Kicks now decode on the kicking thread
    (web only): silent frames 9,999 -> 4,087 in the same run, and the user
    reports clean cutscene audio. A 30 fps cap (`--webgpu_frame_limit=30`)
    did not fix the old shortfall. The mixer still falls short in some
    windows (median 890 of 938 frames per 5 s before the clock fix; not
    measured since).
  - *Experimental:* `--webgpu_early_frame_ack` (off) acknowledges a frame to
    the title at capture instead of at execution. Measured only before the
    XMA fix: somewhat better audio, no clear fps change.
  - *Where the time goes now (Chrome, ~4,900 draws):* render worker ~60% busy
    and the title's render (capturing) thread ~60% busy, each waiting on the
    other part of the time; the capturing thread spends ~1.8 s of 5 s in a
    guest wait (`sub_82A1A450` from `sub_828497D8`), likely for the main
    thread's next frame. Per draw on the render worker: `Draw` itself, the
    WebGPU calls (~2.2 a draw, mostly the texture bind group), `Work`
    destruction, and hash lookups for pipelines and bind groups.
- **Pipeline recipes (2026-10-08).** Every title pipeline's state key is
  saved (`pipeline_recipes.cpp`) and compiled ahead in later runs, so draws
  stop waiting for pipelines that earlier runs already met. `DrawPipeline`
  only builds the key now; `CreatePipeline` builds the descriptor from the key
  alone, so a saved key and a live draw take the same path (bump
  `kRecipeVersion` when the key layout changes). Storage: IndexedDB in the
  browser plus an optional served `pipeline_seed.bin` (for a cold first
  visit), or a file under Node (`--webgpu_pipeline_recipe_file`). Measured
  under Node (Dawn on Metal, 4-minute intro runs): run 1 created ~280
  pipelines with up to 10 draws a frame waiting; run 2 compiled all 281
  recipes in its first 5 s (0 failed), then created no new pipelines and no
  draw waited. Not yet measured in Chrome, where the cold-cache compiles
  (1.5–5 s each) are the real target; look for the `pipeline recipes` perf
  line. The idea came from reading a scraped GTA V web client; no code was
  taken from it.
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
| GPU threading | One render pthread owns the device and runs from the JS event loop (needed for `mapAsync` and canvas presentation), woken via `emscripten_proxy_async`; at most two frames are in flight on the GPU (`TrackGpuFrame`); after each present it yields with a `MessageChannel` message, since a proxied wake can run in the same task and the canvas only updates when the task ends. Game threads capture device block/buffers/textures at submit (`Capture` in `graphics_system.cpp`) | `gta4_webgpu/graphics_system.cpp`, `work.h` |
| Shaders | Stock SPIR-V → GLSL (SPIRV-Cross) → rewrite BDA constants to one UBO (VS 0, PS 4096, shared 8192, spec word 8192+0x500) and bindless to fixed slots → glslang → spirv-opt → naga → WGSL, then split the UBO into three parts (VS, PS, shared + spec word 0x500) read from one group-0 storage buffer at register indices from a two-register per-draw record (`firstInstance` → `instance_index`; the pixel stage's half goes to the pixel shader in a flat varying at location 18); each stage's registers 0–15 have their own hot slot (register r reads `hot + r` below 16, `cold + r` above); dynamic register reads go through `xc_load`. naga undoes the Vulkan y-flip itself. Archive `LRWGSL05` | `tools/webgpu/spirv_to_wgsl.py`, `split_uniforms.py`, `draw_constants.py`, `hot_constants.py`, `LibertyRecompLib/shader/webgpu_shader_archive.bin` |
| Pipelines | Title pipelines by full state key, created with `CreateRenderPipelineAsync`; draws are skipped while pending (`--webgpu_async_pipelines`). Shader modules for registered shaders are made between frames (`WarmModules`, `--webgpu_shader_warmup_ms`), since module creation is synchronous. Keys seen in earlier runs (recipes: IndexedDB, a served seed, or a file under Node) are compiled ahead from the same budget. Utility-pass pipelines stay synchronous | `gta4_webgpu/draw.cpp` (`DrawPipeline`, `CreatePipeline`), `renderer.cpp` (`WarmModules`), `pipeline_recipes.cpp` |
| Vertex data | Every attribute decoded to `float32x4` on the CPU per buffer generation (shaders read vec4 floats; WebGPU cannot feed integer formats to them) | `gta4_webgpu/vertex_decode.h`, `resources.cpp` |
| Render targets | Single-sampled; resolved depth stored as `rg32float` (depth, stencil), with packed A8R8G8B8 aliases rebuilt on demand; resolves pick the latest surface at the same EDRAM placement, and map samples when its MSAA layout differs from the resolved view (`resolve_color`) | `gta4_webgpu/resources.cpp`, `passes.cpp`, `renderer.cpp` |
| Depth handoff | Depth from the resolved snapshot (`source_texture`); `kRebuildSceneCoverage` clears stencil to 0x80 and writes 0xFF via stencil Replace where packed depth is nonzero, as the Metal renderer | `gta4_webgpu/passes.cpp` (`Handoff`) |
| Canvas | `<canvas id="liberty-gpu">` is transferred to the render worker as an OffscreenCanvas (pre-js `res/web/webgpu_canvas.js`); SDL's `#canvas` stays on top for input | `gta4_webgpu/canvas.cpp`, `res/web/index.html` |
| Main loop | `-sPROXY_TO_PTHREAD`; COOP/COEP needed (`tools/web/serve.py`) | `gta4-recomp/CMakeLists.txt` |
| Fused multiply-add | musl's software `fma` replaced by relaxed SIMD `f64x2.relaxed_madd` (module needs relaxed SIMD) | `gta4-recomp/src/web/web_fma.cpp` |
| Multi-object waits | Block on a global signal epoch (futex) instead of 1 ms polling; timer queue uses a blocking wait | `src/core/threading_posix.cpp`, `src/core/timer_queue.cpp` |
| Allocator | `-sMALLOC=mimalloc` (per-thread heaps; dlmalloc's lock stalled draw capture and the renderer). It sits on emmalloc, which also takes memory from `sbrk` and never lowers the break, so the guest region stays valid | `gta4-recomp/CMakeLists.txt` |
| Host clock | `CLOCK_MONOTONIC` counted in nanoseconds at a fixed 1 GHz: browsers report 1000 ns from `clock_getres`, which made guest time run ~1000x fast | `src/core/clock_posix.cpp` |
| XMA kicks | On the web a context kick decodes on the kicking thread instead of waking the decoder thread and waiting (worker path kept while paused) | `src/audio/xma_decoder.cpp` |
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
  `spirv-tools spirv-cross glslang-tools` via apt (or Homebrew), `pip install
  zstandard`. The `tools/XenosRecomp` submodule provides SMOL-V. See
  WEB_BUILDING.md › Shaders. On the user's Mac they are installed: spirv-cross
  and Rust from Homebrew, naga in `~/.cargo/bin` (not on `PATH`; prepend it),
  glslangValidator and spirv-opt in `/usr/local/bin`; `zstandard` needs a venv
  (Homebrew Python is externally managed). The full run takes about 2 minutes.
  To change only the WGSL layer of an existing archive, follow
  `split_uniforms.py`/`draw_constants.py`/`hot_constants.py`: each converts
  an archive in place to the next format version (`hot_constants.py` makes
  `LRWGSL05`); `spirv_to_wgsl.py` applies all of them when it builds one.
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
- `--webgpu_gpu_timing=true` (with `--webgpu_perf_report`) adds GPU busy and
  idle time a frame and the costliest render passes; `--audio_perf_report=true`
  logs the guest mixer's frames (938 per 5 s is real time) and XMA decode time.
- Driving Chrome for measurements: start a separate instance
  (`--user-data-dir=<scratch>/profile --remote-debugging-port=9333
  --enable-webgpu-developer-features`), serve with `tools/web/serve.py`, and
  open `http://localhost:8080/?arg=...` over the DevTools protocol, logging
  `Runtime.consoleAPICalled`; dispatch one mouse click so audio starts. Navigate
  the tab to `about:blank` afterwards: a page left running keeps the game
  going at ~400% CPU and skews builds and later runs.
- Shader hashes map to WGSL in the archive; the archive format is
  `LRWGSL05` (zlib) with per-record hash, stage, variant and code, which a short
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
- Performance: each constants slot (vertex, pixel, shared) is reused while
  its inputs are unchanged, and shaders select slots through `firstInstance`
  (no per-draw bind group); redundant pass state is skipped;
  texture bind groups, samplers and vertex input layouts are cached; converted
  vertex and index data share pooled buffers; draws send only the 128-byte
  chunks of the device block that changed (the render thread keeps its own
  copy); the hot byte compares and swaps use SIMD128 (Emscripten's libc does
  them per byte). Measured in Chrome before the delta/reuse work: ~20–32 µs of
  render-thread time per draw in the 2026-10-07 intro run (6,600 draws: inputs
  36, bindings 25, uniforms 29, encode 39 ms a frame).
  **Benchmark (Node + Dawn on Metal, M1, the 5.5-minute intro, `--webgpu_perf_report`):**

  | Build | Heaviest stretch | fps there | µs per draw (encode / uniforms / inputs / bindings / submit) |
  |---|---|---|---|
  | Before (743d745b) | ~8,400 draws | 4.5–4.8 | ~20 (8.2 / 3.2 / 2.1 / 1.6 / 2.2) |
  | Deltas + uniform reuse + input cache (b08f89bb) | ~6,500 draws | 9.0 | ~13.3 (6.8 / 1.6 / 1.0 / 0.8 / 1.8) |
  | + pooled vertex/index buffers (ec1b0ee8) | ~6,200–8,800 draws | 8.1–11.2 | ~9.5–11.8 (3.9 / 1.5–2.2 / 0.9 / 0.8–1.5 / 1.3–1.7) |
  | + split uniform bindings (20b5a24a) | ~6,600–8,800 draws | 9.3–10.9 (6.5 while compiling pipelines) | ~9.4–10.1 excluding compiles (4.0 / 0.4 / 0.8 / 0.9–1.5 / 1.4) |
  | + two GPU frames in flight (e8e98c5f) | ~6,600–8,800 draws | 8.1–12.1 | unchanged |
  | + async pipelines, shader warm-up | ~6,000–8,900 draws | 8.4–10.6 | ~9.7–11.6; longest frame ≤ 170 ms (was up to 860) |
  | + pass merging, timers only with the flag | ~5,500–8,800 draws | 7–14 (noisy machine) | ~10–15% below the previous row in the same conditions; ~28 passes a frame (was 330–490) |
  | Before constants by `firstInstance` (same 7-min run, prior build) | ~5,600 draws | 13.8–19.1; closing shot 14.0–14.5 | ~7.6 (3.0 / 0.36 / 0.97 / 0.68 / 0.86) |
  | + constants by `firstInstance` (storage buffer, no per-draw bind group) | ~5,600 draws | 13.9–20.1; closing shot 15.0–15.7 | ~6.8 (2.15 / 0.36 / 0.96 / 0.69 / 0.77) |

  Light scenes (300–850 draws) hold the 60 fps cap throughout. Converted
  vertex and index data now live in 32 MB pooled buffers (`BufferPool`): draws
  bind a pool buffer from its start and select their data with
  `firstIndex`/`baseVertex`, so vertex-buffer calls fell from ~1.0 to ~0.25 per
  draw and index-buffer calls from ~0.9 to ~0 (the perf line reports calls per
  draw). The title's constants are three slots (vertex, pixel, shared),
  each reused while unchanged: a new vertex slot is 4 KB where the
  combined slot was ~9.7 KB, and uniforms fell from ~2 to ~0.4 µs a draw.
  Shaders find the slots through a draw record selected by `firstInstance`
  (Where it stands › Constants by `firstInstance`), so there is no
  per-draw bind-group call. A
  flush in the middle of a draw's setup (arena full) makes it set up again in
  the new batch (`redrawn` in the perf line). What remained per draw before the
  `firstInstance` change: ~4 µs of
  encode (mostly the uniform bind group, ~1 call a draw since vertex
  constants change on most draws; now ~2.2 µs, see the table), ~1–2 µs of setup (fixed state,
  shared constants, targets, pipeline key; render passes and the timers
  themselves were most of what was untimed, see Where it stands) and ~1.4 µs of uploads (pixel
  constants change on ~75% as many draws as vertex constants). Pipeline
  compiles no longer stall the render thread (asynchronous creation and the
  shader module warm-up, under Where it stands).
  In Chrome (user report, 2026-10-07, after the split): faster, but the first
  run stalled for about a second at a time, which fast-forwarded parts of the
  opening cutscene and froze gameplay. A second run was much smoother and
  briefly reached playable frame rates in gameplay, with stutter: Chrome
  caches compiled shaders by their WGSL, so any change to the archive makes
  the next run compile every pipeline again. Asynchronous pipeline creation
  and the shader module warm-up removed the stalls in Chrome (longest frame
  ≤ 140 ms with the render thread busy ≤ 111 ms of it), but a cold cache still
  takes 1.5–5 s to compile each new pipeline, so their draws are missing for
  that long and the guest audio mixer falls behind meanwhile (Where it stands
  › Chrome recheck 2026-10-08). Pipeline recipes (Where it stands) now compile
  pipelines seen in earlier runs ahead of their first draw; not yet measured
  in Chrome.
  At most two frames are in flight on the GPU (`TrackGpuFrame`): a present
  waits for an earlier frame to finish, so the render thread cannot queue
  frames behind slow GPU work. The perf report's second line gives the
  window's longest frame (render-thread busy and pipeline time in it), GPU
  frame latency (submit to completion, as seen by the render thread) and how
  many presents waited for the GPU.
  Measured since (CPU session of 2026-10-08): the benchmark table above
  predates mimalloc, hot/cold constants and sampled timers. In Chrome the
  heavy intro scenes (~6,000–7,300 draws) run ~24–27 fps and gameplay ~13–20
  fps at 2,600–7,500 draws; both are CPU-bound.
- Audio: with the XMA kick and clock fixes the guest mixer held real time in
  the Chrome run checked afterwards (median 938 of 938 frames per 5 s; a few
  windows down to ~730), and the user reports clean cutscene audio. Earlier
  runs also underran while Chrome compiled a burst of new pipelines, worst
  with a cold shader cache; not rechecked since (pipeline recipes should
  shorten those bursts on later runs).
- The opening cutscene skipped to gameplay once in Chrome (first run after
  an archive change) and could not be reproduced; cause unknown (Where it
  stands › Chrome recheck 2026-10-08).
- The shader archive is zlib (11 MB); zstd would be 4.7 MB but needs a wasm zstd.
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

1. **WebGPU renderer performance.** Heavy scenes are CPU-bound, not
   GPU-bound (CPU session of 2026-10-08 under Where it stands). Next: measure
   gameplay (not only the intro) with the new tools, find what the title's
   threads wait on, cut per-draw render-worker cost (bind-group calls, hash
   lookups, `Work` allocation), and the capture cost on the title's thread.
   First, check pipeline recipes in Chrome: load the page twice with
   `--webgpu_perf_report=true` and compare draws waiting and audio underruns
   in the opening shots; a fresh profile with `pipeline_seed.bin` served
   tests the cold-cache case (record a longer seed under Node, through
   gameplay, with `--webgpu_pipeline_recipe_file`).
   The older notes below predate that session.
   The renderer is done as a title-command
   renderer (the same interface the desktop gta4-native and gta4-metal
   renderers use, not Xenos emulation), and Chrome renders the intro correctly
   (see above). Per-draw cost is down to ~10 µs (Node, Dawn on Metal; see
   Known gaps › Performance), and pipelines are created asynchronously with
   shader modules made ahead of use and pipelines from earlier runs compiled
   ahead (recipes); draws share render passes (~28 a frame).
   The per-draw uniform bind-group call is gone (constants by
   `firstInstance`, under Where it stands).
   If the GPU does become the limit, the costliest pass is the 1024x768
   RGBA16F one (pixel shader 7B14CAC2A31D4199). Later: close the fidelity
   gaps above.
2. **In-browser game files.** On hold: the user prefers serving the
   installed folder with `serve.py` as now. Note for later: the page's main
   thread does every file read (Emscripten's JS filesystem lives there, and
   reads are synchronous XHR with a byte-by-byte `responseText` decode), so
   it competes with logging, input and SDL's audio callback. If that shows up
   in profiles, move reads to an IO worker that game threads wait on through
   shared memory. A file or folder picker (File System Access API /
   OPFS), mounted so the existing `gta4::install::Install()` can read it;
   reuse the non-interactive install path in `GTA4App::OnFinalizePaths`. Files
   are 7–8 GB, so stream them rather than preloading into MEMFS. (Local runs
   already stream an installed game from `serve.py`; the lazy mount in
   `res/web/index.html` is a model for this.)
3. **Audio, input and page lifecycle.** Audio holds real time in Chrome
   after the XMA kick and clock fixes (pipeline-compile bursts not rechecked);
   the one-off intro skip is still open. `--webgpu_early_frame_ack` is
   experimental and off; retest it before enabling. Then pointer lock (a DevTools-protocol click got `WrongDocumentError`;
   not checked with a real click) and fullscreen.
