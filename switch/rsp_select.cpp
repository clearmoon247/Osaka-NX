// RSP microcode selection for the Switch runner: src/main/main.cpp get_rsp_microcode() without the capture/replay
// wrappers (aspMain_capture, audcmd_record). Order matters: the GB Tower programs are recognised by their boot
// code before the generic task types.
#include <cstdint>
#include <cstdio>

#include "librecomp/rsp.hpp"
#include "ultramodern/ultra64.h"

extern RspUcodeFunc aspMain;
extern RspUcodeFunc gbTowerMain;
extern RspUcodeFunc gbTowerColorMain;
extern RspUcodeFunc njpgdspMain;

namespace {
    // rdram holds big-endian words in host-swapped byte order (address ^ 3 per byte).
    bool rdram_be_word_matches(uint8_t* rdram, uint32_t vaddr, uint32_t expected) {
        constexpr uint32_t kRdramSize = 0x800000u;
        if (rdram == nullptr) return false;
        const uint32_t paddr = vaddr & 0x1FFFFFFFu;
        if (paddr + 4 > kRdramSize) return false;
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; i++) {
            value = (value << 8) | rdram[(paddr + i) ^ 3];
        }
        return value == expected;
    }

    bool task_boot_matches(uint8_t* rdram, const OSTask* task, uint32_t boot_size, uint32_t word0, uint32_t word1) {
        const uint32_t boot = static_cast<uint32_t>(task->t.ucode_boot);
        return boot != 0 && static_cast<uint32_t>(task->t.ucode_boot_size) == boot_size &&
               rdram_be_word_matches(rdram, boot + 0, word0) && rdram_be_word_matches(rdram, boot + 4, word1);
    }
}

// Diagnostics: how many RSP tasks of each kind were started (index: 0 gbTowerMain, 1 gbTowerColorMain, 2 aspMain, 3 njpg, 4 other).
extern "C" {
volatile uint64_t psrnx_rsp_task_counts[5] = {0, 0, 0, 0, 0};
}

RspUcodeFunc* psrnx_get_rsp_microcode(uint8_t* rdram, const OSTask* task) {
    if (task_boot_matches(rdram, task, 0xA40, 0x401A6000u, 0xC800207Cu)) { psrnx_rsp_task_counts[0] = psrnx_rsp_task_counts[0] + 1; return gbTowerMain; }
    if (task_boot_matches(rdram, task, 0x9E4, 0x401A6000u, 0xC800207Cu)) { psrnx_rsp_task_counts[1] = psrnx_rsp_task_counts[1] + 1; return gbTowerColorMain; }
    switch (task->t.type) {
        case M_AUDTASK: psrnx_rsp_task_counts[2] = psrnx_rsp_task_counts[2] + 1; return aspMain;
        case M_NJPEGTASK: psrnx_rsp_task_counts[3] = psrnx_rsp_task_counts[3] + 1; return njpgdspMain;
        default:
            psrnx_rsp_task_counts[4] = psrnx_rsp_task_counts[4] + 1;
            std::fprintf(stderr, "[PSRNX] Unknown RSP task type: %u\n", task->t.type);
            return nullptr;
    }
}
