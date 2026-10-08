// The generated code turns every PowerPC fmadd-family instruction into
// std::fma, which wasm lowers to a call to libc's fma. There is no scalar
// fused multiply-add instruction in wasm, so musl computes it in software
// with wide integer arithmetic, which made it about a third of the CPU time of
// the title's audio mixer thread. This definition replaces musl's (an object
// file wins over the libc archive member) with relaxed SIMD's fused
// multiply-add, which browsers run as one hardware FMA where the CPU has one
// (ARM64, x86-64 with FMA3) and as a multiply then add otherwise.

#include <wasm_simd128.h>

extern "C" double fma(double x, double y, double z) {
  return wasm_f64x2_extract_lane(
      wasm_f64x2_relaxed_madd(wasm_f64x2_splat(x), wasm_f64x2_splat(y), wasm_f64x2_splat(z)), 0);
}
