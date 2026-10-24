/*
 * gbtower_dump_switch.c: text dump of the embedded Game Boy emulator's state for the Switch port's GB Tower watcher.
 *
 * extras.c (upstream) keeps a ring of GB Tower checkpoints (pkmnstadium_gbtower_state_trace) that only the Windows debug
 * server can read. This file reads the same ring through its public accessors and peeks the emulator's context in guest
 * memory, so the port can write it to the SD card. The struct below must match extras.c's gbtower_trace_ev exactly.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp.h"

extern int32_t* section_addresses;
uint64_t pkmnstadium_gbtower_trace_seq(void);
uint32_t pkmnstadium_gbtower_trace_cap(void);
uint32_t pkmnstadium_frag9_base(void);
uint32_t pkmnstadium_frag9_peek32(uint8_t* rdram, uint32_t link_offset);
uint32_t recomp_rsp_get_sp_status(void);

struct gbtower_trace_ev {
    uint64_t seq;
    uint32_t tag;
    uint32_t s0;
    uint32_t ra;
    uint32_t sp;
    uint32_t r2, r3, r4, r5, r6, r7, r8;
    uint32_t r9, r10, r11, r12, r13, r14, r15;
    uint32_t r16, r17, r18, r19, r20, r21, r22, r23, r24, r25;
    uint32_t state;
    uint32_t phase;
    uint32_t g_c4f4;
    uint32_t g_c4f8;
    uint32_t g_c4e0;
    uint32_t g_c4e4;
    uint32_t g_c4e8;
    uint32_t g_c4ec;
    uint32_t g_c4f0;
    uint32_t g_c4fc;
    uint32_t g_b2b8;
    uint32_t s_ctrl0;
    uint32_t s_ctrl1;
    uint32_t s_ctrl2;
    uint32_t s_5dd0;
    uint32_t s_hdr0;
    uint32_t s_hdr1;
    uint32_t s_hdr2;
    uint32_t s_ready0;
    uint32_t s_ready1;
    uint32_t s_pad0;
    uint32_t w5390, w5394, w5398;
    uint32_t w53a4, w53b4, w53bc;
    uint32_t rom_0140, rom_0144, rom_0148, rom_014c;
    uint32_t rom_type;
    uint32_t w5d64, w5d68, w5d6c, w5d70, w5d74, w5d78, w5d7c;
    uint32_t ctx_r2_byte;
    uint32_t gb_pc;
    uint32_t gb_pc_base;
    uint32_t gb_pc_addr;
    uint32_t gb_pc_byte;
    uint32_t op_table0;
    uint32_t op_handler;
    uint32_t io_ff40_43;
    uint32_t io_ff44_47;
    uint32_t io_ff00_03, io_ff04_07, io_ff0c_0f;
    uint32_t io_ff4c_4f, io_ff50_53, io_ff54_57;
    uint32_t hram_ff80_83, hram_ff84_87, hram_ff88_8b;
    uint32_t hram_ffd4_d7, hram_fffc_ff;
    uint32_t h53d6, h53d8, h53da, h53dc, h53de;
    uint32_t w53c0, w53c4, w53c8;
    uint32_t saved_5c44, saved_5c48, saved_5c4c, saved_5c50, saved_5c54;
    uint32_t w5388, w538c, w53b8, w53cc, w53d0, w53f0, w53f2_53f5;
    uint32_t w5484, w5488, w548c;
    uint32_t w55ac, w55b0, w55b4, w55b8, w55bc, w55c0, w55c4, w55c8;
    uint32_t w55ec, w55f0, w55f4, w55f8, w55fc, w5600, w5604, w5608;
    uint32_t w576c;
    uint32_t g_c4e2, g_c4f6, g_c4f7, g_c4f9;
    uint32_t cart_type_flags;
};

#define GBTOWER_TRACE_CAP_LOCAL 32768
void pkmnstadium_gbtower_trace_get(uint32_t i, struct gbtower_trace_ev* out);

static int vaddr_to_paddr32(uint32_t vaddr, uint32_t* out_paddr) {
    uint32_t paddr;
    if ((vaddr >= 0x80000000u && vaddr < 0x80800000u) || (vaddr >= 0xA0000000u && vaddr < 0xA0800000u)) {
        paddr = vaddr & 0x7FFFFFu;
    } else if (vaddr < 0x800000u) {
        paddr = vaddr;
    } else {
        return 0;
    }
    if (paddr >= 0x800000u) {
        return 0;
    }
    *out_paddr = paddr;
    return 1;
}

static uint8_t load_be8_v(uint8_t* rdram, uint32_t vaddr) {
    uint32_t paddr = 0;
    if (!vaddr_to_paddr32(vaddr, &paddr)) {
        return 0;
    }
    return rdram[paddr ^ 3u];
}

static uint16_t load_be16_v(uint8_t* rdram, uint32_t vaddr) {
    uint32_t paddr = 0;
    if (!vaddr_to_paddr32(vaddr, &paddr) || paddr + 1u >= 0x800000u) {
        return 0;
    }
    return (uint16_t)(((uint16_t)rdram[paddr ^ 3u] << 8) | (uint16_t)rdram[(paddr + 1u) ^ 3u]);
}

static uint32_t load_be32_v(uint8_t* rdram, uint32_t vaddr) {
    uint32_t paddr = 0;
    if (!vaddr_to_paddr32(vaddr, &paddr) || paddr + 3u >= 0x800000u) {
        return 0;
    }
    return ((uint32_t)rdram[(paddr + 0) ^ 3] << 24) | ((uint32_t)rdram[(paddr + 1) ^ 3] << 16) |
           ((uint32_t)rdram[(paddr + 2) ^ 3] << 8) | (uint32_t)rdram[(paddr + 3) ^ 3];
}

static uint32_t frag9_load32(uint8_t* rdram, uint32_t link_offset) { return pkmnstadium_frag9_peek32(rdram, link_offset); }
static uint8_t frag9_load8(uint8_t* rdram, uint32_t link_offset) {
    return load_be8_v(rdram, pkmnstadium_frag9_base() + link_offset);
}

/* Writes the newest `tail` GB Tower trace events as text, with a header of live emulator state. Used by the Switch port's
 * watcher thread (gbtower_trace.txt) to see where the embedded Game Boy emulator stands when a cart sits at its logo. */
int pkmnstadium_gbtower_trace_dump(uint8_t* rdram, const char* path, int tail) {
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        return 0;
    }
    uint64_t seq = pkmnstadium_gbtower_trace_seq();
    uint32_t s0 = frag9_load32(rdram, 0x2B2C0u);
    fprintf(f, "# GB Tower trace: %llu events so far, newest %d below; fragment base 0x%08X, emulator context 0x%08X\n",
            (unsigned long long)seq, tail, pkmnstadium_frag9_base(), s0);
    fprintf(f, "# sp_status mirror=0x%08X model=0x%08X  cpu_step_gate(0x2C512)=0x%02X  state(0x2C4DC)=%u phase(0x2C4FC)=%u\n",
            (uint32_t)MEM_W(0, 0xFFFFFFFFA4040010ull), recomp_rsp_get_sp_status(),
            frag9_load8(rdram, 0x2C512u), frag9_load32(rdram, 0x2C4DCu), frag9_load8(rdram, 0x2C4FCu));
    if (s0 != 0) {
        uint32_t gb_pc = load_be16_v(rdram, s0 + 0x53E8u);
        uint32_t base = load_be32_v(rdram, s0 + 0x55ACu + ((gb_pc >> 10) & 0x3Cu));
        fprintf(f, "# saved GB state: pc=0x%04X (byte at pc 0x%02X, page base 0x%08X) 53E4=%04X 53E6=%04X 53EA=%04X regs(5487..548B)=%02X %02X %02X %02X %02X\n",
                gb_pc, load_be8_v(rdram, base + gb_pc), base, load_be16_v(rdram, s0 + 0x53E4u),
                load_be16_v(rdram, s0 + 0x53E6u), load_be16_v(rdram, s0 + 0x53EAu),
                load_be8_v(rdram, s0 + 0x5487u), load_be8_v(rdram, s0 + 0x5488u), load_be8_v(rdram, s0 + 0x5489u),
                load_be8_v(rdram, s0 + 0x548Au), load_be8_v(rdram, s0 + 0x548Bu));
        fprintf(f, "# GB io: ff00..0f=");
        for (uint32_t i = 0; i < 16; i++) fprintf(f, "%02X ", load_be8_v(rdram, s0 + 0x100u + i));
        fprintf(f, " ff40..4f=");
        for (uint32_t i = 0; i < 16; i++) fprintf(f, "%02X ", load_be8_v(rdram, s0 + 0x140u + i));
        fprintf(f, " ff50=%02X ie(ffff)=%02X\n", load_be8_v(rdram, s0 + 0x150u), load_be8_v(rdram, s0 + 0x1FFu));
        fprintf(f, "# bytes at GB pc: ");
        for (uint32_t i = 0; i < 24; i++) fprintf(f, "%02X ", load_be8_v(rdram, base + gb_pc + i));
        fprintf(f, "\n# loader: 5DC7=%02X 5DC8=%02X 5DC9=%02X 5DCD=%02X  5388(rsp-wait flag)=%08X  53DC(scanline*8)=%04X  53A4=%08X 53C4=%08X\n",
                load_be8_v(rdram, s0 + 0x5DC7u), load_be8_v(rdram, s0 + 0x5DC8u), load_be8_v(rdram, s0 + 0x5DC9u),
                load_be8_v(rdram, s0 + 0x5DCDu), load_be32_v(rdram, s0 + 0x5388u), load_be16_v(rdram, s0 + 0x53DCu),
                load_be32_v(rdram, s0 + 0x53A4u), load_be32_v(rdram, s0 + 0x53C4u));
    }
    uint64_t start = seq > (uint64_t)tail ? seq - (uint64_t)tail : 0;
    for (uint64_t i = start; i < seq; i++) {
        struct gbtower_trace_ev ev;
        pkmnstadium_gbtower_trace_get((uint32_t)(i % pkmnstadium_gbtower_trace_cap()), &ev);
        const struct gbtower_trace_ev* e = &ev;
        if (e->seq != i) {
            continue;
        }
        fprintf(f, "#%llu tag=%08X st=%u ph=%u gbpc=%04X op=%02X ff40=%08X ff44=%08X ff50=%08X ctrl=%02X/%02X/%02X 5388=%08X "
                   "53dc=%04X 53a4=%08X c4e0..fc=%08X %08X %08X %08X %08X %08X %08X %08X r2=%08X r3=%08X ra=%08X\n",
                (unsigned long long)e->seq, e->tag, e->state, e->phase & 0xFFu, e->gb_pc, e->gb_pc_byte & 0xFFu,
                e->io_ff40_43, e->io_ff44_47, e->io_ff50_53, e->s_ctrl0 >> 24, e->s_ctrl1 >> 24, e->s_ctrl2 >> 24, e->w5388,
                e->h53dc, e->w53a4, e->g_c4e0, e->g_c4e4, e->g_c4e8, e->g_c4ec, e->g_c4f0, e->g_c4f4, e->g_c4f8, e->g_c4fc,
                e->r2, e->r3, e->ra);
    }
    fclose(f);
    return 1;
}


/* ---- Build 27 diagnostics -------------------------------------------------------------------------------------------
 * The Game Boy CPU loop (tag 0x8120A73C) reports once per Game Boy opcode; the stock hook returns immediately for it. The
 * link-time wrap below keeps that cheap but counts every call and builds a histogram of the Game Boy program counter, so a
 * snapshot taken while the cart sits at its logo shows where the Game Boy program really spends its time. The remaining tags
 * are counted (small table) and forwarded to the stock hook unchanged. */
extern void __real_pkmnstadium_gbtower_state_trace(uint8_t* rdram, uint32_t tag, uint32_t s0, const void* raw_ctx);
extern void recomp_rsp_diag_counters(uint64_t* out6);
extern volatile uint64_t psrnx_rsp_task_counts[5];

volatile uint64_t psrnx_gb_dispatch_count;   /* tag 0x8120A73C */
volatile uint64_t psrnx_gb_9fb4_count;       /* tag 0x81209FB4 */
volatile uint64_t psrnx_gb_init_count;       /* tag 0x81209870 (Game Boy CPU init) */
volatile uint32_t psrnx_gb_last_pc;
static uint32_t g_gb_pc_hist[65536];
static uint32_t g_gb_pc_hist_prev[65536];
#define GB_TAG_SLOTS 48
static struct { uint32_t tag; uint64_t n; } g_gb_tags[GB_TAG_SLOTS];

static uint32_t gb_ctx_early(uint8_t* rdram) { return frag9_load32(rdram, 0x2B2C0u); }
#define gb_ctx gb_ctx_early

/* ---- Boot-ROM hand-off (the GB Tower stall at the Nintendo logo) ---------------------------------------------------------
 * The embedded Game Boy emulator first runs the Game Boy boot ROM (Nintendo logo scroll + chime). When the boot ROM writes
 * FF50 the emulator parks the CPU in a one-frame-at-a-time yield loop (0x8120ACD4) and expects the frame function
 * (func_8120572C) to notice "saved Game Boy pc == 0x100", apply the cart patches (func_81203C58) and re-initialise the CPU in
 * cartridge mode (func_81209870). That check (0x81206434) only runs while the main state is 6, i.e. while the loader is still
 * busy reading the cart over the Transfer Pak. On a real Transfer Pak that outlasts the boot ROM; with the port's instant
 * Transfer Pak the loader finishes first, the state drops to 0 mid-boot and the check never runs again: the CPU sits at 0x100
 * forever. This performs the very same hand-off from state 0 once the boot ROM is done (same steps, same order). */
extern void func_81203C58(uint8_t* rdram, recomp_context* ctx);
extern void func_81209870(uint8_t* rdram, recomp_context* ctx);

volatile uint32_t psrnx_gb_handoff_count;
volatile uint32_t psrnx_gb_handoff_wait;   /* frames spent waiting at pc 0x100 for the conditions below */

static int paddr_ok(uint32_t vaddr, uint32_t* out) { return vaddr_to_paddr32(vaddr, out); }
static void store_be8_v(uint8_t* rdram, uint32_t vaddr, uint8_t v) {
    uint32_t p;
    if (paddr_ok(vaddr, &p)) rdram[p ^ 3u] = v;
}
static void store_be32_v(uint8_t* rdram, uint32_t vaddr, uint32_t v) {
    store_be8_v(rdram, vaddr + 0, (uint8_t)(v >> 24));
    store_be8_v(rdram, vaddr + 1, (uint8_t)(v >> 16));
    store_be8_v(rdram, vaddr + 2, (uint8_t)(v >> 8));
    store_be8_v(rdram, vaddr + 3, (uint8_t)v);
}

static void gb_call_guest(uint8_t* rdram, const recomp_context* live, void (*fn)(uint8_t*, recomp_context*), uint32_t a0,
                          uint32_t ret_addr) {
    recomp_context c = *live;
    c.r4 = (int64_t)(int32_t)a0;
    c.r31 = (int64_t)(int32_t)ret_addr;
    c.host_return_target = ret_addr;
    c.tailcall_pending = 0;
    c.tailcall_target = 0;
    c.tailcall_func = 0;
    fn(rdram, &c);
}

static void gb_boot_handoff_check(uint8_t* rdram, const recomp_context* live, uint32_t s0) {
    if (s0 == 0) return;
    if (frag9_load32(rdram, 0x2C4DCu) != 0) return;            /* main state 0 (run); state 6 does it itself */
    if (frag9_load8(rdram, 0x2C4F6u) != 0) return;             /* hand-off already done */
    if (load_be16_v(rdram, s0 + 0x53E8u) != 0x0100u) return;   /* boot ROM not finished */
    uint32_t hdr = load_be32_v(rdram, s0 + 0x53BCu);
    if (load_be32_v(rdram, s0 + 0x55ACu) != hdr) return;       /* page 0 not yet switched to the cart */
    psrnx_gb_handoff_wait++;
    if (load_be8_v(rdram, s0 + 0x549Cu) == 0 || load_be8_v(rdram, s0 + 0x549Du) == 0 || load_be8_v(rdram, s0 + 0x559Cu) == 0 ||
        load_be8_v(rdram, s0 + 0x559Du) == 0 || load_be8_v(rdram, s0 + 0x559Eu) == 0 || load_be8_v(rdram, s0 + 0x559Fu) == 0) {
        if (psrnx_gb_handoff_wait == 1 || psrnx_gb_handoff_wait % 120 == 0)
            printf("[PSRNX] gbtower: boot ROM done but cart banks not ready (549C/D=%02X/%02X 559C..F=%02X %02X %02X %02X)\n",
                   load_be8_v(rdram, s0 + 0x549Cu), load_be8_v(rdram, s0 + 0x549Du), load_be8_v(rdram, s0 + 0x559Cu),
                   load_be8_v(rdram, s0 + 0x559Du), load_be8_v(rdram, s0 + 0x559Eu), load_be8_v(rdram, s0 + 0x559Fu));
        return;
    }
    printf("[PSRNX] gbtower: boot ROM finished (pc=0x100) in main state 0: running the cart hand-off (patches + CPU re-init)\n");
    if (frag9_load8(rdram, 0x2C4E2u) == 0) store_be8_v(rdram, pkmnstadium_frag9_base() + 0x2C4F9u, 3);
    store_be8_v(rdram, pkmnstadium_frag9_base() + 0x2C4F6u, 1);
    gb_call_guest(rdram, live, func_81203C58, s0, 0x812064C0u);
    hdr = load_be32_v(rdram, s0 + 0x53BCu);
    store_be8_v(rdram, s0 + 0x5485u, (load_be8_v(rdram, hdr + 0x143u) & 0x80u) == 0 ? 1u : 0u);
    uint32_t type = load_be8_v(rdram, hdr + 0x147u);
    store_be32_v(rdram, s0 + 0x5390u, frag9_load32(rdram, 0xD90Cu + type * 4u));
    store_be32_v(rdram, s0 + 0x5394u, 0x100000u);
    gb_call_guest(rdram, live, func_81209870, s0, 0x81206510u);
    psrnx_gb_handoff_count++;
    printf("[PSRNX] gbtower: hand-off done (cart type 0x%02X, flags 0x%08X, pc now 0x%04X)\n", type,
           load_be32_v(rdram, s0 + 0x5390u), load_be16_v(rdram, s0 + 0x53E8u));
}

void __wrap_pkmnstadium_gbtower_state_trace(uint8_t* rdram, uint32_t tag, uint32_t s0, const void* raw_ctx) {
    if (tag == 0x81206648u && raw_ctx != NULL) {
        gb_boot_handoff_check(rdram, (const recomp_context*)raw_ctx, s0 != 0 ? s0 : gb_ctx_early(rdram));
    }
    if (tag == 0x8120A73Cu) {
        const recomp_context* ctx = (const recomp_context*)raw_ctx;
        uint32_t pc = ctx != NULL ? (uint32_t)(ctx->r22 & 0xFFFFu) : 0u;
        g_gb_pc_hist[pc]++;
        psrnx_gb_last_pc = pc;
        psrnx_gb_dispatch_count++;
        return;
    }
    if (tag == 0x81209FB4u) {
        psrnx_gb_9fb4_count++;
        return;
    }
    if (tag == 0x81209870u) {
        psrnx_gb_init_count++;
    }
    for (int i = 0; i < GB_TAG_SLOTS; i++) {
        if (g_gb_tags[i].tag == tag) { g_gb_tags[i].n++; break; }
        if (g_gb_tags[i].tag == 0) { g_gb_tags[i].tag = tag; g_gb_tags[i].n = 1; break; }
    }
    __real_pkmnstadium_gbtower_state_trace(rdram, tag, s0, raw_ctx);
}


/* One compact line with everything that decides whether the emulator can make progress. */
void psrnx_gbtower_state_line(uint8_t* rdram, char* buf, size_t n) {
    uint32_t s0 = gb_ctx(rdram);
    uint64_t sig[6];
    recomp_rsp_diag_counters(sig);
    uint32_t sc = 0x800A62E0u;
    snprintf(buf, n,
        "gb: disp=%llu pc=%04X init=%llu handoff=%u/%u c4f6=%02X | main=%u menu=%02X c4f0=%08X b2f0=%u b2b8=%08X | ld(c4c5c6/c7/c8/c9/cd)=%02X%02X%02X/%02X/%02X/%02X/%02X cd=%08X "
        "| 5388=%08X 53a4=%08X 53c4=%08X ly*8=%04X slice=%04X spc=%04X | lcdc=%02X ly=%02X if=%02X ie=%02X "
        "| sp mirror=%08X model=%08X sig set3/clr3/set2/clr2/all/forced=%llu/%llu/%llu/%llu/%llu/%llu "
        "| rsp tasks gb=%llu col=%llu aud=%llu "
        "| sched A14=%08X A18=%08X A1C=%08X A20=%08X A24=%08X frames=%u prenmi=%u "
        "| t1 fl=%04X st=%04X t2 fl=%04X st=%04X",
        (unsigned long long)psrnx_gb_dispatch_count, (unsigned)psrnx_gb_last_pc, (unsigned long long)psrnx_gb_init_count,
        (unsigned)psrnx_gb_handoff_count, (unsigned)psrnx_gb_handoff_wait, (unsigned)frag9_load8(rdram, 0x2C4F6u),
        frag9_load32(rdram, 0x2C4DCu), frag9_load8(rdram, 0x2C4F7u), frag9_load32(rdram, 0x2C4F0u),
        frag9_load32(rdram, 0x2B2F0u), frag9_load32(rdram, 0x2B2B8u),
        s0 ? load_be8_v(rdram, s0 + 0x5DC4u) : 0, s0 ? load_be8_v(rdram, s0 + 0x5DC5u) : 0,
        s0 ? load_be8_v(rdram, s0 + 0x5DC6u) : 0, s0 ? load_be8_v(rdram, s0 + 0x5DC7u) : 0,
        s0 ? load_be8_v(rdram, s0 + 0x5DC8u) : 0, s0 ? load_be8_v(rdram, s0 + 0x5DC9u) : 0,
        s0 ? load_be8_v(rdram, s0 + 0x5DCDu) : 0, s0 ? load_be32_v(rdram, s0 + 0x5DD0u) : 0,
        s0 ? load_be32_v(rdram, s0 + 0x5388u) : 0, s0 ? load_be32_v(rdram, s0 + 0x53A4u) : 0,
        s0 ? load_be32_v(rdram, s0 + 0x53C4u) : 0, s0 ? load_be16_v(rdram, s0 + 0x53DCu) : 0,
        s0 ? load_be16_v(rdram, s0 + 0x53DEu) : 0, s0 ? load_be16_v(rdram, s0 + 0x53E8u) : 0,
        s0 ? load_be8_v(rdram, s0 + 0x140u) : 0, s0 ? load_be8_v(rdram, s0 + 0x144u) : 0,
        s0 ? load_be8_v(rdram, s0 + 0x10Fu) : 0, s0 ? load_be8_v(rdram, s0 + 0x1FFu) : 0,
        (uint32_t)MEM_W(0, 0xFFFFFFFFA4040010ull), recomp_rsp_get_sp_status(),
        (unsigned long long)sig[0], (unsigned long long)sig[1], (unsigned long long)sig[2], (unsigned long long)sig[3],
        (unsigned long long)sig[4], (unsigned long long)sig[5],
        (unsigned long long)psrnx_rsp_task_counts[0], (unsigned long long)psrnx_rsp_task_counts[1],
        (unsigned long long)psrnx_rsp_task_counts[2],
        load_be32_v(rdram, sc + 0xA14u), load_be32_v(rdram, sc + 0xA18u), load_be32_v(rdram, sc + 0xA1Cu),
        load_be32_v(rdram, sc + 0xA20u), load_be32_v(rdram, sc + 0xA24u), load_be32_v(rdram, sc + 0xA34u),
        (unsigned)load_be16_v(rdram, sc + 0xA38u),
        load_be16_v(rdram, 0x80148D38u + 0x1Cu), load_be16_v(rdram, 0x80148D38u + 0x1Eu),
        load_be16_v(rdram, 0x80148DA0u + 0x1Cu), load_be16_v(rdram, 0x80148DA0u + 0x1Eu));
}

static void print_hex(FILE* f, uint8_t* rdram, uint32_t vaddr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) fprintf(f, "%02X ", load_be8_v(rdram, vaddr + i));
}

/* Full snapshot file: state line, tag counts, Game Boy pc histogram (total and since the previous snapshot), the code around the
 * hottest pcs, the cart header as the emulator sees it, then the newest checkpoints (existing dump). */
int psrnx_gbtower_snapshot(uint8_t* rdram, const char* path) {
    if (!pkmnstadium_gbtower_trace_dump(rdram, path, 150)) {
        return 0;
    }
    FILE* f = fopen(path, "a");
    if (f == NULL) {
        return 0;
    }
    char line[2048];
    psrnx_gbtower_state_line(rdram, line, sizeof(line));
    fprintf(f, "\n# ---- build 27 snapshot ----\n%s\n", line);
    fprintf(f, "# checkpoint tag counts:");
    for (int i = 0; i < GB_TAG_SLOTS && g_gb_tags[i].tag != 0; i++)
        fprintf(f, " %08X=%llu", g_gb_tags[i].tag, (unsigned long long)g_gb_tags[i].n);
    fprintf(f, "\n# dispatch loop total=%llu, 9FB4 total=%llu\n", (unsigned long long)psrnx_gb_dispatch_count,
            (unsigned long long)psrnx_gb_9fb4_count);

    static uint32_t delta[65536];
    for (uint32_t i = 0; i < 65536; i++) delta[i] = g_gb_pc_hist[i] - g_gb_pc_hist_prev[i];
    uint32_t s0 = gb_ctx(rdram);
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t* h = pass == 0 ? delta : g_gb_pc_hist;
        fprintf(f, "# top Game Boy pcs %s:\n", pass == 0 ? "since the previous snapshot" : "since start");
        static uint32_t taken[65536 / 32];
        memset(taken, 0, sizeof(taken));
        for (int k = 0; k < 24; k++) {
            uint32_t best = 0, bestn = 0;
            for (uint32_t i = 0; i < 65536; i++) {
                if (h[i] > bestn && !(taken[i >> 5] & (1u << (i & 31)))) { bestn = h[i]; best = i; }
            }
            if (bestn == 0) break;
            taken[best >> 5] |= 1u << (best & 31);
            fprintf(f, "#   pc=%04X hits=%u  bytes: ", best, bestn);
            if (s0) {
                uint32_t base = load_be32_v(rdram, s0 + 0x55ACu + ((best >> 10) & 0x3Cu));
                fprintf(f, "(page base %08X) ", base);
                print_hex(f, rdram, base + best, 12);
            }
            fprintf(f, "\n");
        }
    }
    memcpy(g_gb_pc_hist_prev, g_gb_pc_hist, sizeof(g_gb_pc_hist));
    if (s0) {
        uint32_t base0 = load_be32_v(rdram, s0 + 0x55ACu);
        fprintf(f, "# page0 base=%08X; ROM 0000..003F: ", base0);
        print_hex(f, rdram, base0, 64);
        fprintf(f, "\n# ROM 0100..014F: ");
        print_hex(f, rdram, base0 + 0x100u, 80);
        fprintf(f, "\n# page table 55AC..55EC: ");
        for (uint32_t i = 0; i < 16; i++) fprintf(f, "%08X ", load_be32_v(rdram, s0 + 0x55ACu + i * 4u));
        fprintf(f, "\n# HRAM FF80..FFFF: ");
        print_hex(f, rdram, s0 + 0x180u, 128);
        fprintf(f, "\n# io FF00..FF7F: ");
        print_hex(f, rdram, s0 + 0x100u, 128);
        fprintf(f, "\n");
    }
    fclose(f);
    return 1;
}
