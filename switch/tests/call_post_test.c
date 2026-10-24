// Checks that the call-site helpers in call_post_switch.c behave exactly like the code the recompiler emits (the template
// text in tools/compact_calls.py), on random states: same control-flow outcome, same context afterwards.
//   gcc -O1 -I../lib/N64ModernRuntime/N64Recomp/include tests/call_post_test.c call_post_switch.c -o /tmp/call_post_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp.h"
#define recomp_cf_note(...) ((void)0)

int psr_after_call(uint8_t* rdram, recomp_context* ctx, gpr sp0, uint32_t prev, uint32_t ret);
int psr_after_call_nl(uint8_t* rdram, recomp_context* ctx, gpr sp0, uint32_t prev);
int psr_call(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn, uint32_t ret);
int psr_call_nl(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn);

// What the "callee" and the tail-call dispatcher do is driven by a script held in globals, so both sides see the same thing.
static struct { int pend; uint32_t target; uint32_t disp; gpr sp_after; int hp_clear; int hp_sp_after; int calls, hcalls; } sc;
static uint32_t RET = 0x8433CC24u;
static void mock_callee(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; sc.calls++;
    ctx->tailcall_pending = sc.pend; ctx->tailcall_target = sc.target; ctx->tailcall_func = sc.pend ? (recomp_func_t*)0x1234 : 0;
    ctx->tailcall_dispatching = sc.disp; ctx->r29 = sc.sp_after; ctx->r2 = 77;
}
void recomp_handle_tailcalls(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; sc.hcalls++;
    if (sc.hp_clear) { ctx->tailcall_pending = 0; ctx->tailcall_target = 0; ctx->tailcall_func = 0; }
    if (sc.hp_sp_after) ctx->r29 += 8;
    ctx->r3 = 55;
}

// The recompiler's text, verbatim, with "goto L_x" turned into "return 2" and the end of the block into "return 0".
static int orig_ret(uint8_t* rdram, recomp_context* ctx) {
    {
    gpr recomp_call_sp = ctx->r29;
    uint32_t recomp_prev_host_return = ctx->host_return_target;
    uint32_t recomp_call_host_return = (uint32_t)ctx->r31;
    ctx->host_return_target = recomp_call_host_return;
    mock_callee(rdram, ctx);
    if (ctx->tailcall_pending) {
        if (ctx->tailcall_target == 0x8433CC24u) {
            recomp_cf_note("call-return-tailcall", (uint32_t)recomp_call_sp, recomp_prev_host_return, recomp_call_host_return, ctx);
            ctx->tailcall_pending = 0;
            ctx->tailcall_target = 0;
            ctx->tailcall_func = 0;
            ctx->host_return_target = recomp_prev_host_return;
            if (ctx->r29 != recomp_call_sp) { recomp_cf_note("call-sp-mismatch", (uint32_t)recomp_call_sp, recomp_prev_host_return, recomp_call_host_return, ctx); return 1; }
            return 2;
        }
        if (ctx->tailcall_dispatching >= 64u) {
            recomp_cf_note("call-bubble-dispatch", (uint32_t)recomp_call_sp, recomp_prev_host_return, recomp_call_host_return, ctx);
            ctx->host_return_target = recomp_prev_host_return;
            return 1;
        }
        recomp_handle_tailcalls(rdram, ctx);
    }
    ctx->host_return_target = recomp_prev_host_return;
    if (ctx->r29 != recomp_call_sp) { recomp_cf_note("call-sp-mismatch", (uint32_t)recomp_call_sp, recomp_prev_host_return, recomp_call_host_return, ctx); return 1; }
    }
    return 0;
}
static int orig_nl(uint8_t* rdram, recomp_context* ctx) {
    {
    gpr recomp_call_sp = ctx->r29;
    uint32_t recomp_prev_host_return = ctx->host_return_target;
    uint32_t recomp_call_host_return = (uint32_t)ctx->r31;
    ctx->host_return_target = recomp_call_host_return;
    mock_callee(rdram, ctx);
    if (ctx->tailcall_pending) {
        if (ctx->tailcall_dispatching >= 64u) {
            recomp_cf_note("call-bubble-dispatch", (uint32_t)recomp_call_sp, recomp_prev_host_return, recomp_call_host_return, ctx);
            ctx->host_return_target = recomp_prev_host_return;
            return 1;
        }
        recomp_handle_tailcalls(rdram, ctx);
    }
    ctx->host_return_target = recomp_prev_host_return;
    if (ctx->r29 != recomp_call_sp) { recomp_cf_note("call-sp-mismatch", (uint32_t)recomp_call_sp, recomp_prev_host_return, recomp_call_host_return, ctx); return 1; }
    }
    return 0;
}
// The three rewritten forms.
static int new_wrap(uint8_t* rdram, recomp_context* ctx) { if (psr_call(rdram, ctx, mock_callee, 0x8433CC24u)) return 1; return 0; }
static int new_post(uint8_t* rdram, recomp_context* ctx) {
    gpr recomp_call_sp = ctx->r29; uint32_t recomp_prev_host_return = ctx->host_return_target;
    ctx->host_return_target = (uint32_t)ctx->r31;
    mock_callee(rdram, ctx);
    switch (psr_after_call(rdram, ctx, recomp_call_sp, recomp_prev_host_return, 0x8433CC24u)) { case 1: return 1; case 2: return 2; }
    return 0;
}
static int new_wrap_nl(uint8_t* rdram, recomp_context* ctx) { if (psr_call_nl(rdram, ctx, mock_callee)) return 1; return 0; }
static int new_post_nl(uint8_t* rdram, recomp_context* ctx) {
    gpr recomp_call_sp = ctx->r29; uint32_t recomp_prev_host_return = ctx->host_return_target;
    ctx->host_return_target = (uint32_t)ctx->r31;
    mock_callee(rdram, ctx);
    if (psr_after_call_nl(rdram, ctx, recomp_call_sp, recomp_prev_host_return)) return 1;
    return 0;
}

// Call site with the local-label switch (three labels), and its rewritten form.
static const uint32_t tbl3[] = { 0x8433CC70u, 0x8433CC80u, 0x8433CC90u, 0 };
static int orig_local(uint8_t* rdram, recomp_context* ctx) {
    {
    gpr recomp_call_sp = ctx->r29;
    uint32_t recomp_prev_host_return = ctx->host_return_target;
    uint32_t recomp_call_host_return = (uint32_t)ctx->r31;
    ctx->host_return_target = recomp_call_host_return;
    mock_callee(rdram, ctx);
    if (ctx->tailcall_pending) {
        if (ctx->tailcall_target == 0x8433CC24u) {
            recomp_cf_note("call-return-tailcall", (uint32_t)recomp_call_sp, recomp_prev_host_return, recomp_call_host_return, ctx);
            ctx->tailcall_pending = 0;
            ctx->tailcall_target = 0;
            ctx->tailcall_func = 0;
            ctx->host_return_target = recomp_prev_host_return;
            if (ctx->r29 != recomp_call_sp) { recomp_cf_note("call-sp-mismatch", (uint32_t)recomp_call_sp, recomp_prev_host_return, recomp_call_host_return, ctx); return 1; }
            return 0;
        }
        if (ctx->tailcall_target != 0) {
            switch ((uint32_t)ctx->tailcall_target) {
            case 0x8433CC70u:
                ctx->tailcall_pending = 0; ctx->tailcall_target = 0; ctx->tailcall_func = 0;
                ctx->host_return_target = recomp_prev_host_return;
                if (ctx->r29 != recomp_call_sp) { return 1; }
                return 100;
            case 0x8433CC80u:
                ctx->tailcall_pending = 0; ctx->tailcall_target = 0; ctx->tailcall_func = 0;
                ctx->host_return_target = recomp_prev_host_return;
                if (ctx->r29 != recomp_call_sp) { return 1; }
                return 101;
            case 0x8433CC90u:
                ctx->tailcall_pending = 0; ctx->tailcall_target = 0; ctx->tailcall_func = 0;
                ctx->host_return_target = recomp_prev_host_return;
                if (ctx->r29 != recomp_call_sp) { return 1; }
                return 102;
            default: break;
            }
        }
        if (ctx->tailcall_dispatching >= 64u) {
            ctx->host_return_target = recomp_prev_host_return;
            return 1;
        }
        recomp_handle_tailcalls(rdram, ctx);
    }
    ctx->host_return_target = recomp_prev_host_return;
    if (ctx->r29 != recomp_call_sp) { return 1; }
    }
    return 0;
}
int psr_call_l(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn, uint32_t ret, const uint32_t* tbl);
static int new_local(uint8_t* rdram, recomp_context* ctx) {
    int r = psr_call_l(rdram, ctx, mock_callee, 0x8433CC24u, tbl3);
    if (r == 0 || r == 1) return r;
    return 100 + (r - 2);
}

int main(void) {
    srand(12345);
    long n = 0, bad = 0, outcomes[3] = {0, 0, 0}, hits_handle = 0;
    uint32_t targets[] = {0, 0x8433CC24u, 0x1000u, 0x8433CC28u, 0x8433CC70u, 0x8433CC80u, 0x8433CC90u};
    uint32_t disps[] = {0, 1, 63, 64, 65, 1000};
    for (long i = 0; i < 400000; i++) {
        sc.pend = rand() & 1; sc.target = targets[rand() % 7]; sc.disp = disps[rand() % 6];
        gpr sp_before = 0x80100000ull + (rand() & 0xF0);
        sc.sp_after = (rand() % 3) ? sp_before : sp_before + 8;
        sc.hp_clear = rand() & 1; sc.hp_sp_after = rand() % 4 == 0;
        recomp_context a; memset(&a, 0, sizeof a);
        a.r29 = sp_before; a.r31 = RET; a.host_return_target = (uint32_t)rand();
        a.tailcall_dispatching = (uint32_t)(rand() % 4);
        recomp_context b = a, c = a, d = a, e = a, f = a, g = a, h = a;
        {   // local-label call site
            recomp_context p = a, q = a; sc.calls = sc.hcalls = 0;
            int r0 = orig_local(0, &p); int h0 = sc.hcalls; sc.calls = sc.hcalls = 0; int r1 = new_local(0, &q);
            n++; if (r0 >= 100) outcomes[2]++;
            if (r0 != r1 || h0 != sc.hcalls || memcmp(&p, &q, sizeof p) != 0) { if (bad < 5) fprintf(stderr, "MISMATCH local: %d vs %d\n", r0, r1); bad++; }
        }
        for (int mode = 0; mode < 4; mode++) {
            int r0, r1; recomp_context *x, *y;
            sc.calls = sc.hcalls = 0;
            if (mode == 0) { r0 = orig_ret(0, &a); int h0 = sc.hcalls; sc.calls = sc.hcalls = 0; r1 = new_post(0, &b); if (r1 == 2 && r0 != 2) bad++; (void)h0; if (h0 != sc.hcalls) bad++; x = &a; y = &b; }
            else if (mode == 1) { r0 = orig_ret(0, &c); int h0 = sc.hcalls; sc.calls = sc.hcalls = 0; r1 = new_wrap(0, &d); if (r0 == 2) r0 = 0; if (h0 != sc.hcalls) bad++; x = &c; y = &d; }
            else if (mode == 2) { r0 = orig_nl(0, &e); int h0 = sc.hcalls; sc.calls = sc.hcalls = 0; r1 = new_post_nl(0, &f); if (h0 != sc.hcalls) bad++; x = &e; y = &f; }
            else { r0 = orig_nl(0, &g); int h0 = sc.hcalls; sc.calls = sc.hcalls = 0; r1 = new_wrap_nl(0, &h); if (h0 != sc.hcalls) bad++; x = &g; y = &h; }
            n++; if (mode == 0) outcomes[r0]++; if (sc.hcalls) hits_handle++;
            if (r0 != r1 || memcmp(x, y, sizeof *x) != 0) { if (bad < 5) fprintf(stderr, "MISMATCH mode %d: outcome %d vs %d (pend=%d tgt=%X disp=%u)\n", mode, r0, r1, sc.pend, sc.target, sc.disp); bad++; }
        }
    }
    printf("%ld comparisons (outcomes ret=%ld/continue=%ld/label=%ld, handle_tailcalls used %ld times): %s\n",
           n, outcomes[1], outcomes[0], outcomes[2], hits_handle, bad ? "MISMATCHES" : "all identical");
    return bad != 0;
}
