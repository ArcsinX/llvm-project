// SPDX-License-Identifier: MIT
// Copyright (c) 2026 DeepSeek
//
// clangd shim header for Ascend CCE (include/cce_stubs/cce_stubs.h).
// Provides builtin type stubs, includes the bisheng runtime wrapper,
// and makes Ascend CCE headers parseable by our custom clangd.

#ifndef __CCE_STUBS_H__
#define __CCE_STUBS_H__

// Editor identity. Hardware architecture and core selection belong in the
// compilation database or .clangd. Never infer an NPU from the host CPU.
#ifndef __CCE__
#define __CCE__ 1
#endif
#ifndef __BISHENG_CCEC__
#define __BISHENG_CCEC__ 1
#endif
#define __CLANGD_ASCENDC__ 1

// CCE builtin types (__hif8, __fp8e4m3, etc.) are now implemented as
// genuine clang built-in types in our custom clangd build.

// Pre-empt __clang_cce_defines.h — provide our own definitions that map
// CCE address space qualifiers to clang's __attribute__((address_space(N)))
// so that differently-qualified types are treated as distinct types.
#define __CCE_DEFINES_H__

#define __no_return__ __attribute__((noreturn))
#define __sync_noalias__
#define __sync_alias__
#define __check_sync_alias__
#define __sync_in__
#define __sync_out__
#define __in_pipe__(...)
#define __out_pipe__(...)
#define __inout_pipe__(...)
#define __forceinline__ __inline__ __attribute__((always_inline))
#define __align__(n) __attribute__((aligned(n)))

#define __global__
#define __simt_callee__
#define __simt_vf__ __attribute__((noinline))
#define __aicpu__
#define __disable_kernel_type_autoinfer__
#define LAUNCH_BOUND(N)
#define __launch_bounds__(N)
#define __schedmode__(N)
#define __maxnreg__(N)

#define __simd_vf__
#define __simd_callee__
#define __no_simd_vf_fusion__
#define __callee__

#define __cube__
#define __vector__
#define __mix__(cube, vec)

// Address space qualifiers — use distinct address_space IDs to make types differ
#ifdef __CCE_STUB_DISABLE_ADDRESS_SPACE_QUALIFIERS__

#define __gm__
#define __ca__
#define __cb__
#define __cc__
#define __ubuf__
#define __cbuf__
#define __fbuf__
#define __biasbuf__
#define __private__
#define __ssbuf__

#else

#define __gm__    __attribute__((address_space(1)))
#define __ca__    __attribute__((address_space(2)))
#define __cb__    __attribute__((address_space(3)))
#define __cc__    __attribute__((address_space(4)))
#define __ubuf__  __attribute__((address_space(5)))
#define __cbuf__  __attribute__((address_space(6)))
#define __fbuf__  __attribute__((address_space(7)))
#define __biasbuf__ __attribute__((address_space(8)))
#define __private__
#define __ssbuf__ __attribute__((address_space(9)))

#endif

#define __copyval__
#define __no_specialization__
#define __device_immutable__
#define __device_builtin__
#define __early_read_before_pre_task_done__
#define __kfc_workspace__
#define __sk__
#define __aicore__
#define __host__

// Provide integer types before the CANN runtime headers are parsed.

// Use the user's SDK if its compiler headers are on the include path.
// No SDK snapshot is distributed with clangd.
#if __has_include("__clang_cce_runtime_wrapper.h")
#define RT_CONFIGURE_CALL
static inline unsigned int __CCE_RT_CONFIGURE_FUNC_NAME__(unsigned int, void *, void *) { return 0; }
#include "cce_builtin_stubs.h"
#include "cce_intrinsic_stubs.h"
#include "cce_simt_stubs.h"
#include "__clang_cce_runtime_wrapper.h"
using dim3 = cce::dim3;
using namespace __cce_scalar;
namespace __asc_aicore {}
using namespace __asc_aicore;
#else
// Minimal declarations for editing standalone kernels without a CANN SDK.
// Full tensor, pipeline, vector and operator APIs require real CANN headers.
using half = __cce_half;
using bfloat16_t = __bf16;
using bfloat16 = __bf16;
struct dim3 {
  unsigned int x, y, z;
  constexpr dim3(unsigned int x = 1, unsigned int y = 1, unsigned int z = 1)
      : x(x), y(y), z(z) {}
};
extern const dim3 gridDim, blockDim, blockIdx, threadIdx;
extern const int warpSize;
extern const unsigned int block_num, block_idx;
// Current SIMD syntax uses a byte count; earlier SDKs accept an SM descriptor.
// Templates keep a literal zero unambiguous while preserving legacy pointers.
unsigned int __cce_rtConfigureCall(unsigned int, decltype(sizeof(0)) = 0,
                                  void * = nullptr);
namespace __clangd_ascendc {
template <class T> struct NullptrConfig {};
template <> struct NullptrConfig<decltype(nullptr)> { using type = unsigned int; };
}
template <class T>
typename __clangd_ascendc::NullptrConfig<T>::type
__cce_rtConfigureCall(unsigned int, T, void *);
template <class T>
unsigned int __cce_rtConfigureCall(unsigned int, T *, void *);
unsigned int __cce_rtConfigureCall(dim3, dim3, decltype(sizeof(0)), void *);
#endif

#endif // __CCE_STUBS_H__
