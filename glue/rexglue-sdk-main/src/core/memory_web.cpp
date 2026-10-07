/**
 * @file        core/memory_web.cpp
 * @brief       WebAssembly host memory primitives
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * WebAssembly linear memory has no page protection, no reservations and no
 * way to map the same pages twice. The guest address space is instead one
 * region taken straight from sbrk (see CreateFileMappingHandle): memory past
 * the current break comes from memory.grow, which the browser zero-fills
 * lazily, and Emscripten's malloc never trims the break, so the region is
 * guaranteed zero and is only committed by the browser as it is touched.
 *
 * Inside that region, reserve/commit and protection changes always succeed
 * without doing anything. Decommit and release zero the range, so a later
 * commit sees zero pages as it would on Windows or with MADV_DONTNEED.
 * Guest aliases are folded by address translation, not by mapping (see
 * rex/memory/web_guest_layout.h).
 */

#include <rex/platform.h>
#if REX_PLATFORM_WEB

#include <rex/memory/utils.h>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace rex {
namespace memory {

namespace {

struct Region {
  uint8_t* base;
  size_t size;
};

std::mutex& state_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<Region>& regions() {
  static std::vector<Region> list;
  return list;
}

// Host allocations made through AllocFixed(nullptr, ...).
std::unordered_set<void*>& host_allocations() {
  static std::unordered_set<void*> set;
  return set;
}

bool InRegion(const void* address, size_t length) {
  auto p = reinterpret_cast<uintptr_t>(address);
  for (const Region& region : regions()) {
    auto base = reinterpret_cast<uintptr_t>(region.base);
    if (p >= base && p - base <= region.size && length <= region.size - (p - base)) {
      return true;
    }
  }
  return false;
}

constexpr size_t kGranularity = 64 * 1024;

}  // namespace

// Guest code works in 4 KB pages; wasm itself has no smaller unit than its
// 64 KB memory page, which is reported as the allocation granularity.
size_t page_size() {
  return 4096;
}

size_t allocation_granularity() {
  return kGranularity;
}

bool IsWritableExecutableMemorySupported() {
  return false;
}

void* AllocFixed(void* base_address, size_t length, AllocationType, PageAccess) {
  std::lock_guard lock(state_mutex());
  if (!base_address) {
    size_t size = std::max<size_t>((length + kGranularity - 1) & ~(kGranularity - 1), kGranularity);
    void* result = std::aligned_alloc(kGranularity, size);
    if (result) {
      std::memset(result, 0, size);
      host_allocations().insert(result);
    }
    return result;
  }
  // Every page of the guest region is permanently backed.
  return InRegion(base_address, length) ? base_address : nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  std::lock_guard lock(state_mutex());
  if (InRegion(base_address, length)) {
    std::memset(base_address, 0, length);
    return true;
  }
  auto it = host_allocations().find(base_address);
  if (it == host_allocations().end()) {
    return false;
  }
  if (deallocation_type == DeallocationType::kRelease) {
    host_allocations().erase(it);
    std::free(base_address);
  } else {
    std::memset(base_address, 0, length);
  }
  return true;
}

bool Protect(void*, size_t, PageAccess, PageAccess* out_old_access) {
  // No page protection exists, so everything is always read/write.
  if (out_old_access) {
    *out_old_access = PageAccess::kReadWrite;
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  std::lock_guard lock(state_mutex());
  access_out = PageAccess::kReadWrite;
  length = page_size();
  return InRegion(base_address, 1) || host_allocations().contains(base_address);
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path&, size_t length, PageAccess,
                                          bool) {
  std::lock_guard lock(state_mutex());
  // Take the region directly past the current break, aligned to the
  // allocation granularity. See the file comment for why it is zero.
  auto current = reinterpret_cast<uintptr_t>(sbrk(0));
  size_t padding = (kGranularity - (current % kGranularity)) % kGranularity;
  void* raw = sbrk(static_cast<intptr_t>(padding + length));
  if (raw == reinterpret_cast<void*>(-1)) {
    return kFileMappingHandleInvalid;
  }
  auto* base = static_cast<uint8_t*>(raw) + padding;
  regions().push_back({base, length});
  return reinterpret_cast<FileMappingHandle>(base);
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path&) {
  std::lock_guard lock(state_mutex());
  // The break cannot be lowered past later allocations, so the region stays
  // allocated for the life of the process; only stop treating it as guest.
  auto& list = regions();
  std::erase_if(
      list, [&](const Region& r) { return reinterpret_cast<FileMappingHandle>(r.base) == handle; });
}

void* MapFileView(FileMappingHandle handle, void*, size_t, PageAccess, size_t file_offset) {
  // Views cannot be placed at chosen addresses; the caller uses the returned
  // pointer into the region.
  if (handle == kFileMappingHandleInvalid) {
    return nullptr;
  }
  return reinterpret_cast<uint8_t*>(handle) + file_offset;
}

bool UnmapFileView(FileMappingHandle, void*, size_t) {
  return true;
}

}  // namespace memory
}  // namespace rex

#endif  // REX_PLATFORM_WEB
