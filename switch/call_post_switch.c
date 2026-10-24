// Shared call-site helpers for the generated code. switch/tools/compact_calls.py rewrites the ~20 lines the recompiler emits
// after every guest call into one call of these, which cuts the compiled code by about a quarter. Each function is the
// original block, statement for statement (see the template in compact_calls.py); tests/call_post_test.c checks that against
// the original text on random states.
//
// psr_call / psr_call_nl:   made the call themselves; return nonzero when the caller must return.
// psr_after_call / _nl:     only the part after the call (used where the recompiler jumps to a label other than the one
//                           that follows the call). 0 = continue, 1 = return, 2 = goto the call's return label.
#include "recomp.h"

static inline int post_common(uint8_t* rdram, recomp_context* ctx, gpr sp0, uint32_t prev) {
    if (ctx->tailcall_pending) {
        if (ctx->tailcall_dispatching >= 64u) {
            ctx->host_return_target = prev;
            return 1;
        }
        recomp_handle_tailcalls(rdram, ctx);
    }
    ctx->host_return_target = prev;
    return ctx->r29 != sp0;
}

int psr_after_call(uint8_t* rdram, recomp_context* ctx, gpr sp0, uint32_t prev, uint32_t ret) {
    if (ctx->tailcall_pending && ctx->tailcall_target == ret) {
        ctx->tailcall_pending = 0;
        ctx->tailcall_target = 0;
        ctx->tailcall_func = 0;
        ctx->host_return_target = prev;
        return ctx->r29 != sp0 ? 1 : 2;
    }
    return post_common(rdram, ctx, sp0, prev);
}

int psr_after_call_nl(uint8_t* rdram, recomp_context* ctx, gpr sp0, uint32_t prev) {
    return post_common(rdram, ctx, sp0, prev);
}

int psr_call(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn, uint32_t ret) {
    gpr sp0 = ctx->r29;
    uint32_t prev = ctx->host_return_target;
    ctx->host_return_target = (uint32_t)ctx->r31;
    fn(rdram, ctx);
    return psr_after_call(rdram, ctx, sp0, prev, ret) == 1;
}

// For call sites of a function that has local labels reachable by a tail call. tbl lists the guest addresses of those labels
// (zero-terminated). Returns 0 = continue, 1 = return, 2 + i = continue at the i-th label (the caller dispatches).
int psr_call_l(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn, uint32_t ret, const uint32_t* tbl) {
    gpr sp0 = ctx->r29;
    uint32_t prev = ctx->host_return_target;
    ctx->host_return_target = (uint32_t)ctx->r31;
    fn(rdram, ctx);
    if (ctx->tailcall_pending) {
        uint32_t target = ctx->tailcall_target;
        if (target == ret) {
            ctx->tailcall_pending = 0;
            ctx->tailcall_target = 0;
            ctx->tailcall_func = 0;
            ctx->host_return_target = prev;
            return ctx->r29 != sp0;            // 1 = return, 0 = go on at the return label, which is where the caller continues
        }
        if (target != 0) {
            for (uint32_t i = 0; tbl[i] != 0; i++) {
                if (tbl[i] == target) {
                    ctx->tailcall_pending = 0;
                    ctx->tailcall_target = 0;
                    ctx->tailcall_func = 0;
                    ctx->host_return_target = prev;
                    return ctx->r29 != sp0 ? 1 : 2 + i;
                }
            }
        }
    }
    return post_common(rdram, ctx, sp0, prev);
}

int psr_call_nl(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn) {
    gpr sp0 = ctx->r29;
    uint32_t prev = ctx->host_return_target;
    ctx->host_return_target = (uint32_t)ctx->r31;
    fn(rdram, ctx);
    return post_common(rdram, ctx, sp0, prev);
}
