#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWD_PERF_REQUEST_BUCKET_COUNT 16U
#define SWD_PERF_WAIT_STREAK_BUCKET_COUNT 6U

typedef struct {
    uint64_t transfer_cycles;
    uint64_t read_transfer_cycles;
    uint64_t write_transfer_cycles;
    uint64_t target_drive_cycles;
    uint64_t host_drive_cycles;
    uint64_t ack_ok_cycles;
    uint64_t ack_wait_cycles;
    uint64_t ack_fault_cycles;
    uint64_t ack_error_cycles;
    uint64_t ack_invalid_cycles;
    uint32_t transfer_count;
    uint32_t read_transfer_count;
    uint32_t write_transfer_count;
    uint32_t target_drive_count;
    uint32_t host_drive_count;
    uint32_t ack_ok_count;
    uint32_t ack_wait_count;
    uint32_t ack_fault_count;
    uint32_t ack_error_count;
    uint32_t ack_invalid_count;
    uint32_t request_count[SWD_PERF_REQUEST_BUCKET_COUNT];
    uint32_t request_ok_count[SWD_PERF_REQUEST_BUCKET_COUNT];
    uint32_t request_wait_count[SWD_PERF_REQUEST_BUCKET_COUNT];
    uint32_t retry_call_count;
    uint32_t retry_wait_count;
    uint32_t retry_max_waits;
    uint32_t retry_wait_streaks[SWD_PERF_WAIT_STREAK_BUCKET_COUNT];
    uint32_t min_transfer_cycles;
    uint32_t max_transfer_cycles;
} swd_perf_counters_t;

void swd_perf_reset_counters(void);
void swd_perf_get_counters(swd_perf_counters_t *counters);
void swd_perf_record_retry(uint32_t request, uint32_t wait_count,
                           uint8_t final_ack);

#ifdef __cplusplus
}
#endif
