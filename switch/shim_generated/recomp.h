// Put in front of the real recomp.h for the GENERATED code only (see switch/CMakeLists.txt, RecompiledFuncs).
//
// 1. The recompiler emits four recomp_cf_note(...) calls at every call site. recomp_cf_note is a control-flow diagnostics probe:
//    it logs only when PSR_CFDIAG is set, and its one always-on effect is a counter that only the PC debug server reads. With
//    the calls in place, each call site carries about twice the code, so this build compiles them out. The runtime's own
//    definition of recomp_cf_note (librecomp/src/recomp.cpp) is unaffected because that file does not see this header.
//    Set PSR_KEEP_CF_NOTE=1 when running switch/build.sh to keep the probe (e.g. to debug control flow with PSR_CFDIAG).
// 2. The call-site helpers that switch/tools/compact_calls.py puts into the generated files (call_post_switch.c).
#include_next "recomp.h"

#ifndef PSR_KEEP_CF_NOTE
#define recomp_cf_note(...) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif
int psr_call(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn, uint32_t ret);
int psr_call_nl(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn);
int psr_call_l(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn, uint32_t ret, const uint32_t* tbl);
int psr_after_call(uint8_t* rdram, recomp_context* ctx, gpr sp0, uint32_t prev, uint32_t ret);
int psr_after_call_nl(uint8_t* rdram, recomp_context* ctx, gpr sp0, uint32_t prev);
#ifdef __cplusplus
}
#endif
