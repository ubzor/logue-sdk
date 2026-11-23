/*
 *  miniz_common.h -- self-contained common types for the inflate-only slice
 *  of miniz (tinfl). Public domain (see the "unlicense" statement at the end
 *  of miniz_tinfl.c / miniz.h).
 *
 *  This is a TRIMMED copy of miniz 3.1.x's miniz_common.h: only the scalar
 *  types + macros the tinfl low-level API needs are kept. The ZIP / deflate /
 *  PNG / stdio / time_t / allocation-function machinery of the original
 *  miniz_common.h is dropped so this header stays dependency-free and can be
 *  included from an embedded (no-libc, no-libm) audio unit.
 *
 *  Why a slice instead of the full amalgamation: the device only ever
 *  DECOMPRESSes the (small, < 16 KB) wavetable-editor document sent from the
 *  web simulator. It never compresses, reads ZIP, or writes PNG. Pulling the
 *  full miniz.c (deflate + zip + png + stdio) would bloat the unit's ROM for
 *  code it cannot reach; the tinfl core is the whole of what we link.
 */

#ifndef MINIZ_COMMON_H_
#define MINIZ_COMMON_H_

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* --- scalar types ------------------------------------------------------- */
typedef unsigned char mz_uint8;
typedef int16_t mz_int16;
typedef uint16_t mz_uint16;
typedef uint32_t mz_uint32;
typedef uint32_t mz_uint;
typedef uint64_t mz_uint64;
typedef int32_t mz_int;
typedef int mz_bool;

#define MZ_FALSE (0)
#define MZ_TRUE (1)

#if defined(NO_INLINE)
#define MZ_FORCEINLINE
#elif defined(_MSC_VER)
#define MZ_FORCEINLINE __forceinline
#elif defined(__GNUC__)
#define MZ_FORCEINLINE __inline__ __attribute__((__always_inline__))
#else
#define MZ_FORCEINLINE inline
#endif

#define MZ_MACRO_END while (0, 0)

#define MZ_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MZ_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MZ_CLEAR_OBJ(obj) memset(&(obj), 0, sizeof(obj))
#define MZ_CLEAR_ARR(arr) memset((arr), 0, sizeof(arr))
#define MZ_CLEAR_ARR_SIZE(arr, size) memset((arr), 0, (size))

/* Little-endian reads. The portable (byte-wise) forms are used unconditionally:
 * they are alignment-safe (the input pointer from the wire packet need not be
 * 4-byte aligned) and behave identically on the Cortex-M7, emscripten-wasm and
 * any other target. This avoids the MINIZ_USE_UNALIGNED_LOADS_AND_STORES /
 * MINIZ_LITTLE_ENDIAN feature test the original miniz_common.h carries. */
#define MZ_READ_LE16(p) ((mz_uint32)(((const mz_uint8 *)(p))[0]) | ((mz_uint32)(((const mz_uint8 *)(p))[1]) << 8U))
#define MZ_READ_LE32(p) ((mz_uint32)(((const mz_uint8 *)(p))[0]) | ((mz_uint32)(((const mz_uint8 *)(p))[1]) << 8U) | ((mz_uint32)(((const mz_uint8 *)(p))[2]) << 16U) | ((mz_uint32)(((const mz_uint8 *)(p))[3]) << 24U))

/* Assertions. The tinfl core uses MZ_ASSERT in exactly one place: to flag that
 * the decoder read past the end of the input stream (corrupt input). We map it
 * to a no-op rather than assert(): (a) an audio unit must not drag the libc
 * __assert_func symbol into its dynamic symbol table (the NTS-1 runtime only
 * resolves unit_* + the firmware LUTs -- see the Makefile's check-und guard),
 * and (b) corrupt input is rejected upstream by the wire-format CRC32 check in
 * drone-osc/osc_wte_wire.h, so a tinfl assertion can never be the first line of
 * defence. A failed decode therefore surfaces as TINFL_STATUS_FAILED (the
 * caller checks the return value), not as an abort. */
#define MZ_ASSERT(x) ((void)0)

/* --- allocation --------------------------------------------------------- */
/* The tinfl low-level API (tinfl_decompress / tinfl_decompress_mem_to_mem)
 * performs NO allocation: the decompressor context and output buffer are both
 * supplied by the caller. MZ_MALLOC/MZ_FREE are only referenced by the higher-
 * level heap helpers, which we do NOT compile (see miniz_tinfl.c). They are
 * kept as no-ops so a stray reference cannot drag malloc into the unit. */
#define MZ_MALLOC(x) NULL
#define MZ_FREE(x) (void)x, ((void)0)
#define MZ_REALLOC(p, x) NULL

/* 64-bit bit buffer: use a 64-bit accumulator on 64-bit targets (faster
 * bit reads), 32-bit otherwise. The original miniz derives this from
 * __x86_64__ et al.; replicate the same test here. On the ARM Cortex-M7
 * (32-bit) and in emscripten-wasm (32-bit) this is 0 -> 32-bit bitbuf. */
#if defined(__x86_64__) || defined(__ppc64__) || defined(_M_X64) || defined(__aarch64__) || (defined(__WORDSIZE) && __WORDSIZE == 64)
#define MINIZ_HAS_64BIT_REGISTERS 1
#else
#define MINIZ_HAS_64BIT_REGISTERS 0
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* MINIZ_COMMON_H_ */