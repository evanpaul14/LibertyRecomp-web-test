/**
 * @file        rex/core/fiber_web.cpp
 * @brief       WebAssembly backend for rex::thread::Fiber (thread fibers only)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * WebAssembly cannot capture or switch native stacks: Emscripten's fibers
 * need Asyncify, which is impractical for a recompiled binary this large, and
 * JSPI stack switching is not yet usable for this. Every guest thread still
 * converts itself to a fiber (XThread::Execute), so that path is supported.
 * Creating and switching to separate fibers fails loudly instead. GTA IV
 * does not import the guest fiber APIs.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WEB

#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t, void (*)(void*), void*) {
  std::fprintf(stderr, "[rex] Fiber::Create is not supported on WebAssembly\n");
  return nullptr;
}

void Fiber::SwitchTo(Fiber* target) {
  if (target == tls_current_) {
    return;
  }
  // Only thread fibers exist on the web, so this would require switching to
  // another thread's stack.
  std::fprintf(stderr, "[rex] Fiber::SwitchTo between fibers is not supported on WebAssembly\n");
  std::abort();
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_WEB
