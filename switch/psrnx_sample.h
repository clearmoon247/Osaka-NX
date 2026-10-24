// Stall-sampler record shared by threads_switch.c and main_switch.cpp.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSRNX_BT_MAX 10

typedef struct {
    uint64_t pc, lr, x0, x1;
    uint64_t bt[PSRNX_BT_MAX];     // return addresses from the frame-pointer chain (best effort)
    int nbt;
    int idle;                      // parked in svcWaitSynchronization / svcSleepThread: nothing to report
    // pc is svcArbitrateLock: the thread waits for a libnx mutex (this is what std::mutex / pthread_mutex / newlib locks use)
    int blocked_on_mutex;
    uint64_t mutex_addr;
    uint32_t owner_handle;
    char owner_name[24];
    uint64_t owner_pc, owner_lr;
    uint64_t owner_bt[PSRNX_BT_MAX];
    int owner_nbt;
} PsrnxSample;

int psrnx_thread_count(void);
const char* psrnx_thread_name(int);
int psrnx_sample_thread_ex(int index, PsrnxSample* out);

#ifdef __cplusplus
}
#endif
