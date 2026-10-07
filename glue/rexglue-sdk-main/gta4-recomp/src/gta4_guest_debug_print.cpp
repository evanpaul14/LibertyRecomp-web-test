// GTA IV's debug-print routine (sub_822BCA90) is compiled to an empty stub in
// the retail executable, so the title's own diagnostics (including the
// message printed right before a fatal-error spin) are lost. With
// gta4_log_guest_debug_print enabled, log each message instead. Only the
// integer, character and string conversions are expanded; anything else is
// printed as written.

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "gta4_init.h"

REXCVAR_DEFINE_BOOL(gta4_log_guest_debug_print, false, "GTA IV/Diagnostics",
                    "Log messages passed to the title's (retail-stripped) debug print routine");

namespace gta4::guest_debug_print {
namespace {

constexpr size_t kMaxStringLength = 512;

std::string ReadGuestString(uint8_t* base, uint32_t address) {
  std::string text;
  if (address < 0x10000) {
    return "(null)";
  }
  for (size_t i = 0; i < kMaxStringLength; ++i) {
    const char c = static_cast<char>(REX_LOAD_U8(address + static_cast<uint32_t>(i)));
    if (c == '\0') {
      break;
    }
    text.push_back(c);
  }
  return text;
}

std::string Format(PPCContext& ctx, uint8_t* base) {
  const std::string format = ReadGuestString(base, ctx.r3.u32);
  // Integer arguments follow the format string in r4-r10.
  const uint32_t args[] = {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                           ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  size_t next = 0;
  std::string out;
  for (size_t i = 0; i < format.size(); ++i) {
    if (format[i] != '%' || i + 1 >= format.size()) {
      out.push_back(format[i]);
      continue;
    }
    size_t spec = i + 1;
    while (spec < format.size() &&
           std::string_view("-+ #0123456789.lh").find(format[spec]) != std::string_view::npos) {
      ++spec;
    }
    if (spec >= format.size()) {
      out.append(format, i);
      break;
    }
    const char conversion = format[spec];
    if (conversion == '%') {
      out.push_back('%');
    } else if (next >= std::size(args)) {
      out.append(format, i, spec - i + 1);
    } else {
      const uint32_t value = args[next++];
      switch (conversion) {
        case 's':
          out += ReadGuestString(base, value);
          break;
        case 'c':
          out.push_back(static_cast<char>(value));
          break;
        case 'd':
        case 'i':
          out += std::to_string(static_cast<int32_t>(value));
          break;
        case 'u':
          out += std::to_string(value);
          break;
        case 'x':
        case 'X':
        case 'p':
          out += fmt::format("{:x}", value);
          break;
        default:
          out.append(format, i, spec - i + 1);
          break;
      }
    }
    i = spec;
  }
  return out;
}

}  // namespace
}  // namespace gta4::guest_debug_print

REX_HOOK_RAW(sub_822BCA90) {
  if (REXCVAR_GET(gta4_log_guest_debug_print)) {
    REXLOG_INFO("guest-print lr={:08X}: {}", static_cast<uint32_t>(ctx.lr),
                gta4::guest_debug_print::Format(ctx, base));
  }
}
