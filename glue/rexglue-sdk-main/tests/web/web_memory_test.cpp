/**
 * @file        tests/web/web_memory_test.cpp
 * @brief       Checks the WebAssembly guest memory layout and MMIO routing
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Built only on Emscripten (target rex-web-memory-test). Run under Node 24+:
 *   node rex-web-memory-test.js
 */

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/ppc/function.h>
#include <rex/system/web_guest_access.h>
#include <rex/system/xmemory.h>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
  if (!condition) {
    ++g_failures;
  }
}

struct FakeRegister {
  uint32_t value = 0x12345678;
  uint32_t last_write_address = 0;
  uint32_t last_write_value = 0;
  int reads = 0;
};

uint32_t ReadRegister(void*, void* context, uint32_t) {
  auto* reg = static_cast<FakeRegister*>(context);
  ++reg->reads;
  return reg->value;
}

void WriteRegister(void*, void* context, uint32_t address, uint32_t value) {
  auto* reg = static_cast<FakeRegister*>(context);
  reg->last_write_address = address;
  reg->last_write_value = value;
}

}  // namespace

int main() {
  using rex::web_guest::Load;
  using rex::web_guest::Store;

  rex::memory::Memory memory;
  if (!memory.Initialize()) {
    std::printf("FAIL Memory::Initialize\n");
    return 1;
  }
  uint8_t* base = memory.virtual_membase();
  Check(memory.physical_membase() == base + 0x100000000ull,
        "physical memory follows the 4 GB window");

  // Fresh guest memory is zero.
  Check(Load<uint32_t>(base, 0x40001000) == 0, "fresh virtual memory reads zero");
  Check(Load<uint64_t>(base, 0xA0400000) == 0, "fresh physical memory reads zero");

  // The physical aliases share one copy (E0 is shifted by 4 KB).
  Store<uint32_t>(base, 0xA0002000, 0xCAFEF00D);
  Check(Load<uint32_t>(base, 0xC0002000) == 0xCAFEF00D, "0xC0000000 aliases 0xA0000000");
  Check(Load<uint32_t>(base, 0xE0001000) == 0xCAFEF00D, "0xE0000000 aliases physical + 4 KB");
  Check(*memory.TranslatePhysical<uint32_t*>(0x2000) == __builtin_bswap32(0xCAFEF00D),
        "TranslatePhysical sees the same big-endian bytes");
  Check(memory.TranslateVirtual(0xC0002000) == memory.TranslateVirtual(0xA0002000) &&
            memory.TranslateVirtual(0xE0001000) == memory.TranslateVirtual(0xA0002000),
        "TranslateVirtual folds every physical view");
  Check(memory.HostToGuestVirtual(memory.TranslateVirtual(0xE0001000)) == 0xA0002000,
        "HostToGuestVirtual reports the canonical 0xA0000000 view");
  Check(rex::memory::GuestPtr(base, 0xC0002000) == memory.TranslateVirtual(0xA0002000),
        "GuestPtr matches TranslateVirtual");

  // Byte order and widths through the generated-code helpers.
  Store<uint64_t>(base, 0x40002000, 0x0102030405060708ull);
  Check(base[0x40002000] == 0x01 && base[0x40002007] == 0x08, "stores are big-endian");
  Check(Load<uint16_t>(base, 0x40002002) == 0x0304, "16-bit load");
  Check(Load<uint8_t>(base, 0x40002007) == 0x08, "8-bit load");
  Check(Load<uint64_t>(base, 0x40002000) == 0x0102030405060708ull, "64-bit round trip");

  // Unaliased ranges stay separate.
  Store<uint32_t>(base, 0x00002000, 0x11111111);
  Check(Load<uint32_t>(base, 0xA0002000) == 0xCAFEF00D, "virtual 0x00000000 is not physical");

  // MMIO: registered registers go to the handler, the rest of the 0x7F block
  // is memory aliased onto physical 0.
  FakeRegister reg;
  Check(memory.AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0xFFFF, &reg, ReadRegister,
                                     WriteRegister),
        "register an MMIO range");
  Store<uint32_t>(base, 0x7FC80010, 0xDEADBEEF);
  Check(reg.last_write_address == 0x7FC80010 && reg.last_write_value == 0xDEADBEEF,
        "MMIO store reaches the handler in host byte order");
  Check(Load<uint32_t>(base, 0x7FC80010) == 0x12345678 && reg.reads == 1,
        "MMIO load comes from the handler");
  Check(*reinterpret_cast<uint32_t*>(rex::web_guest::HostAddress(base, 0x7FC80010)) == 0,
        "MMIO store does not land in memory");
  Store<uint32_t>(base, 0x7F000040, 0xA5A5A5A5);
  Check(Load<uint32_t>(base, 0xA0000040) == 0xA5A5A5A5,
        "unregistered 0x7F000000 writeback aliases physical 0");

  // Heap allocations in physical memory are zero and visible through every view.
  uint32_t phys = memory.SystemHeapAlloc(0x10000, 0x1000, rex::memory::kSystemHeapPhysical);
  Check(phys != 0, "physical SystemHeapAlloc");
  if (phys) {
    Check(Load<uint32_t>(base, phys) == 0, "new physical allocation is zero");
    Store<uint32_t>(base, phys, 0x0BADCAFE);
    uint32_t physical_address = memory.GetPhysicalAddress(phys);
    Check(*memory.TranslatePhysical<uint32_t*>(physical_address) == __builtin_bswap32(0x0BADCAFE),
          "allocation is visible through its physical address");
    memory.SystemHeapFree(phys);
  }

  // Pointers returned by native helpers keep the alias of their argument.
  {
    const uint32_t c0_args[8] = {0xD9000100, 0x2F, 0, 0, 0, 0, 0, 0};
    Check(rex::ppc::detail::HostPointerToGuest(base, rex::memory::GuestPtr(base, 0xD9000105),
                                               c0_args) == 0xD9000105,
          "returned pointer keeps the 0xC0000000 alias of its argument");
    const uint32_t e0_args[8] = {0xF9000100, 0, 0, 0, 0, 0, 0, 0};
    Check(rex::ppc::detail::HostPointerToGuest(base, rex::memory::GuestPtr(base, 0xF9000180),
                                               e0_args) == 0xF9000180,
          "returned pointer keeps the 0xE0000000 alias of its argument");
    const uint32_t virtual_args[8] = {0x40002000, 0, 0, 0, 0, 0, 0, 0};
    Check(rex::ppc::detail::HostPointerToGuest(base, rex::memory::GuestPtr(base, 0x40002004),
                                               virtual_args) == 0x40002004,
          "returned pointer in virtual memory is unchanged");
    Check(rex::ppc::detail::HostPointerToGuest(base, rex::memory::GuestPtr(base, 0xC0000040),
                                               virtual_args) == 0xA0000040,
          "unrelated physical pointer falls back to the canonical view");
  }

  std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
