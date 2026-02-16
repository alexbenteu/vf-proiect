#ifndef Minisat_InstinctGpuShared_h
#define Minisat_InstinctGpuShared_h 
#include <stdint.h>
#include <string.h>
namespace Minisat {
static const uint32_t INSTINCT_GPU_MAGIC = 0x4F4F5243u;
static const uint32_t INSTINCT_GPU_VERSION = 4u;
static const uint32_t INSTINCT_GPU_MAX_VARS = 262144u;
static const uint32_t INSTINCT_GPU_MAX_TARGETS = 256u;
static const uint32_t INSTINCT_GPU_RING_CAPACITY = 64u;
struct InstinctGpuHint {
    uint64_t request_id;
    float confidence;
    float p_conflict_true;
    float p_conflict_false;
    uint32_t decision_level;
    uint32_t assigned_count;
    uint32_t top_decision_var;
    uint8_t top_decision_sign;
    uint8_t has_hint;
    uint8_t prefer_true;
    uint16_t _pad0;
};
struct InstinctGpuRequest {
    uint64_t request_id;
    uint32_t num_vars;
    uint32_t num_targets;
    uint32_t walks;
    uint32_t decision_level;
    uint32_t assigned_count;
    uint32_t top_decision_var;
    uint8_t top_decision_sign;
    uint8_t _pad0[3];
    int8_t assignment[INSTINCT_GPU_MAX_VARS];
    int8_t preferred_phase[INSTINCT_GPU_MAX_VARS];
    uint32_t targets[INSTINCT_GPU_MAX_TARGETS];
};
struct InstinctGpuSharedMemory {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t write_seq;
    volatile uint32_t read_seq;
    volatile uint32_t stop_flag;
    uint32_t _pad0;
    volatile uint64_t latest_processed_request_id;
    uint64_t _pad1;
    InstinctGpuRequest requests[INSTINCT_GPU_RING_CAPACITY];
    InstinctGpuHint hints[INSTINCT_GPU_MAX_VARS];
};
static inline void instinctGpuResetSharedState(InstinctGpuSharedMemory* shm) {
    if (shm == 0)
        return;
    shm->write_seq = 0;
    shm->read_seq = 0;
    shm->stop_flag = 0;
    shm->latest_processed_request_id = 0;
    memset((void*)shm->requests, 0, sizeof(shm->requests));
    memset((void*)shm->hints, 0, sizeof(shm->hints));
}
}
#endif
