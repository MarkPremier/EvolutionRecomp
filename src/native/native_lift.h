/* Support for functions lifted by tools/x86_lift.py (src/native/*.c).
 *
 * Guest memory goes through memcpy rather than the generated code's volatile
 * pointers, so a lifted function's own stack slots can stay in registers
 * between a store and a reload; the base is read through the same const alias
 * the generated code uses (recomp_types.h). The x87 rules are the generated
 * code's: RECOMP_FP_PC's rounding by the control word's precision (pc24 is
 * read at the function's start and after each call, the only places it can
 * change), RECOMP_FCMP_CC's condition codes. */
#ifndef NATIVE_LIFT_H
#define NATIVE_LIFT_H
#include <stdint.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"

#if defined(_MSC_VER)
extern const ptrdiff_t g_xbox_mem_offset_fixed;
#pragma comment(linker, "/alternatename:g_xbox_mem_offset_fixed=g_xbox_mem_offset")
#define NL_BASE g_xbox_mem_offset_fixed
#define NL_INLINE static __forceinline
#else
#define NL_BASE g_xbox_mem_offset
#define NL_INLINE static inline __attribute__((always_inline))
#endif

#define NL_P(a) ((uint8_t *)((uintptr_t)(uint32_t)(a) + NL_BASE))
NL_INLINE uint32_t L32(uint32_t a) { uint32_t v; memcpy(&v, NL_P(a), 4); return v; }
NL_INLINE uint32_t L16(uint32_t a) { uint16_t v; memcpy(&v, NL_P(a), 2); return v; }
NL_INLINE uint32_t L8(uint32_t a) { return *NL_P(a); }
NL_INLINE float LF(uint32_t a) { float v; memcpy(&v, NL_P(a), 4); return v; }
NL_INLINE double LD(uint32_t a) { double v; memcpy(&v, NL_P(a), 8); return v; }
NL_INLINE void S32(uint32_t a, uint32_t v) { memcpy(NL_P(a), &v, 4); }
NL_INLINE void S16(uint32_t a, uint32_t v) { uint16_t w = (uint16_t)v; memcpy(NL_P(a), &w, 2); }
NL_INLINE void S8(uint32_t a, uint32_t v) { *NL_P(a) = (uint8_t)v; }
NL_INLINE void SF(uint32_t a, float v) { memcpy(NL_P(a), &v, 4); }
NL_INLINE void SD(uint32_t a, double v) { memcpy(NL_P(a), &v, 8); }
/* the same, never traced (rep stos) */
NL_INLINE void S32N(uint32_t a, uint32_t v) { memcpy(NL_P(a), &v, 4); }
NL_INLINE void S8N(uint32_t a, uint32_t v) { *NL_P(a) = (uint8_t)v; }

#define PC(x) (pc24 ? recomp_fp_round24(x) : (double)(x))
#define FCMP_CC(a, b) RECOMP_FCMP_CC(RECOMP_FCMP((a), (b)))
#define PARITY8(x) recomp_parity8((uint32_t)(x))

#endif
