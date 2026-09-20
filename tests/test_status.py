#!/usr/bin/env python3
"""Host regression checks for SWD status propagation; run with python3.

Compile the actual host-driver functions with a scripted wire transport. This
requires a host C compiler, not ESP-IDF or attached target hardware.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'interface/swd_host.c').read_text()
names = ['swd_get_apsel', 'int2array', 'swd_transfer_retry']
functions = []
for name in names:
    match = re.search(r'^.*\b' + name + r'\([^\n]*\)\n\{', source, re.M)
    end = source.index('\n}', match.end()) + 2
    functions.append(source[match.start():end])
functions.append(source[source.index('esp_err_t IRAM_ATTR swd_clear_errors'):source.index('esp_err_t swd_init_debug')])

functions.append(source[source.index('esp_err_t swd_flash_syscall_exec_async'):])

shim = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static uint32_t esp_cpu_get_cycle_count(void) { return 0; }
#include "swd_host.h"
#define IRAM_ATTR
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MAX_SWD_RETRY 100
#define TARGET_AUTO_INCREMENT_PAGE_SIZE 1024
#define CSW_VALUE (CSW_RESERVED | CSW_MSTRDBG | CSW_HPROT | CSW_DBGSTAT | CSW_SADDRINC)
#define DBG_Addr 0xe000edf0
#define DCRDR 0xE000EDF8
#define DCRSR 0xE000EDF4
#define DHCSR 0xE000EDF0
#define REGWnR (1 << 16)
typedef struct { uint32_t r[16]; uint32_t xpsr; } DEBUG_STATE;
static struct { uint32_t select, csw; } dap_state;
static unsigned calls, fail_at, wait_remaining;
static uint8_t failure_ack;
static uint32_t read_value;
static unsigned polls, halt_after, delays;
static int64_t now_us, poll_time_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(unsigned ticks) {
    assert(ticks == 1);
    assert(polls == (delays + 1) * CONFIG_ESP_SWD_HALT_POLL_COUNT);
    ++delays;
    now_us += 10000; // Model this project's 100 Hz tick rate.
}
void SWJ_Sequence(uint32_t count, const uint8_t *data) { (void)count; (void)data; }
uint8_t SWD_Transfer(uint32_t req, uint32_t *data) {
    ++calls;
    if (wait_remaining) { --wait_remaining; return DAP_TRANSFER_WAIT; }
    if (fail_at && calls >= fail_at) return failure_ack;
    if (req == (SWD_REG_AP | SWD_REG_R | AP_DRW)) {
        ++polls;
        now_us += poll_time_us;
        if (halt_after && polls >= halt_after) read_value |= S_HALT;
    }
    if ((req & SWD_REG_R) && data) memcpy(data, &read_value, sizeof(read_value));
    return DAP_TRANSFER_OK;
}
static void reset_wire(void) {
    calls = fail_at = wait_remaining = 0;
    polls = halt_after = delays = 0;
    now_us = poll_time_us = 0;
    read_value = 0;
    failure_ack = DAP_TRANSFER_FAULT;
    dap_state.select = dap_state.csw = UINT32_MAX;
}
'''
tests = r'''
int main(void) {
    uint32_t value = 0x12345678;
    uint8_t buffer[16] = {0};
    reset_wire();
    assert(swd_transfer_retry(0, NULL) == ESP_OK);
    reset_wire(); wait_remaining = 2;
    assert(swd_transfer_retry(0, NULL) == ESP_OK && calls == 3);
    reset_wire(); wait_remaining = MAX_SWD_RETRY;
    assert(swd_transfer_retry(0, NULL) == ESP_ERR_TIMEOUT && calls == MAX_SWD_RETRY);
    const uint8_t acks[] = {DAP_TRANSFER_FAULT, DAP_TRANSFER_ERROR, 0, 7};
    for (unsigned i = 0; i < sizeof(acks); ++i) {
        reset_wire(); fail_at = 1; failure_ack = acks[i];
        assert(swd_read_dp(0, &value) == (i == 0 ? ESP_FAIL : ESP_ERR_INVALID_RESPONSE));
        assert(value == 0x12345678 && calls == 1);
    }
    // Fail each transfer in a complete memory operation, including completion.
    for (int writing = 0; writing < 2; ++writing) {
        reset_wire();
        assert((writing ? swd_write_memory(0x20000001, buffer, 13) :
                          swd_read_memory(0x20000001, buffer, 13)) == ESP_OK);
        unsigned total = calls;
        assert(total == 26); // Baseline: no extra wire operations on success.
        for (unsigned stage = 1; stage <= total; ++stage) {
            reset_wire(); fail_at = stage; failure_ack = DAP_TRANSFER_ERROR;
            assert((writing ? swd_write_memory(0x20000001, buffer, 13) :
                              swd_read_memory(0x20000001, buffer, 13)) == ESP_ERR_INVALID_RESPONSE);
            assert(calls == stage);
        }
    }
    reset_wire(); fail_at = 2;
    assert(swd_write_ap(AP_CSW, 0x42) == ESP_FAIL);
    assert(dap_state.csw == UINT32_MAX);
    fail_at = 0;
    assert(swd_write_ap(AP_CSW, 0x42) == ESP_OK && dap_state.csw == 0x42);
    reset_wire();
    assert(swd_read_word(0, NULL) == ESP_ERR_INVALID_ARG && calls == 0);
    assert(swd_read_memory(0, NULL, 1) == ESP_ERR_INVALID_ARG && calls == 0);
    assert(swd_write_memory(0, NULL, 0) == ESP_OK && calls == 0);
    assert(swd_read_block(0, buffer, 3) == ESP_ERR_INVALID_ARG);
    assert(swd_write_block(0, buffer, 0) == ESP_ERR_INVALID_ARG);
    assert(swd_read_core_register(0, &value) == ESP_ERR_TIMEOUT);
    assert(value == 0x12345678);
    // Preserve the caller's register output at every failed transfer, including
    // DCRDR after a successful register-ready poll. Check both target and wire errors.
    for (unsigned error = 0; error < 2; ++error) {
        reset_wire(); read_value = S_REGRDY;
        assert(swd_read_core_register(0, &value) == ESP_OK && value == S_REGRDY);
        unsigned total = calls;
        assert(total == 12); // Same successful register-read wire cost as before.
        for (unsigned stage = 1; stage <= total; ++stage) {
            reset_wire(); read_value = S_REGRDY;
            fail_at = stage;
            failure_ack = error ? DAP_TRANSFER_ERROR : DAP_TRANSFER_FAULT;
            value = 0x12345678;
            assert(swd_read_core_register(0, &value) ==
                   (error ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL));
            assert(value == 0x12345678 && calls == stage);
        }
    }
    // Sticky target status is a target fault even when all wire ACKs are OK.
    DEBUG_STATE state = {0};
    reset_wire(); read_value = S_REGRDY;
    assert(swd_write_debug_state(&state) == ESP_OK);
    const uint32_t sticky[] = {STICKYERR, WDATAERR};
    for (unsigned i = 0; i < sizeof(sticky) / sizeof(sticky[0]); ++i) {
        reset_wire(); read_value = S_REGRDY | sticky[i];
        assert(swd_write_debug_state(&state) == ESP_FAIL);
    }
    reset_wire(); assert(swd_write_core_register(0, 0) == ESP_ERR_TIMEOUT);
    reset_wire(); assert(swd_wait_until_halted() == ESP_ERR_TIMEOUT);
    assert(now_us == 5000000 && delays == 500);
    assert(polls == 500 * CONFIG_ESP_SWD_HALT_POLL_COUNT);
    reset_wire(); halt_after = CONFIG_ESP_SWD_HALT_POLL_COUNT;
    assert(swd_wait_until_halted() == ESP_OK && delays == 0);
    assert(polls == CONFIG_ESP_SWD_HALT_POLL_COUNT);
    reset_wire(); halt_after = CONFIG_ESP_SWD_HALT_POLL_COUNT + 1;
    assert(swd_wait_until_halted() == ESP_OK && delays == 1);
    assert(polls == CONFIG_ESP_SWD_HALT_POLL_COUNT + 1);
    reset_wire(); poll_time_us = 5000000;
    assert(swd_wait_until_halted() == ESP_ERR_TIMEOUT && polls == 1 && delays == 0);
    reset_wire(); fail_at = 1;
    assert(swd_wait_until_halted() == ESP_FAIL && delays == 0);
    reset_wire(); read_value = S_HALT;
    assert(swd_wait_until_halted() == ESP_OK && polls == 1 && delays == 0);
    reset_wire(); fail_at = 1; failure_ack = DAP_TRANSFER_WAIT;
    assert(swd_read_word(0, &value) == ESP_ERR_TIMEOUT);
    reset_wire();
    assert(swd_flash_syscall_exec_async(NULL, 0, 0, 0, 0, 0) == ESP_ERR_INVALID_ARG);
    assert(swd_flash_syscall_wait_result((flash_algo_return_t)99, NULL) == ESP_ERR_INVALID_ARG);
    read_value = S_HALT | S_REGRDY;
    assert(swd_flash_syscall_wait_result(FLASHALGO_RETURN_POINTER, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(swd_flash_syscall_wait_result(FLASHALGO_RETURN_BOOL, NULL) == ESP_FAIL);
    assert(swd_flash_syscall_wait_result(FLASHALGO_RETURN_VALUE, &value) == ESP_OK);
    assert(value == read_value);
    puts("PASS: ACK mapping, retries, nested read/write error propagation, output preservation, cache recovery, arguments, polling timeouts and burst yielding");
}
'''
with tempfile.TemporaryDirectory(prefix='swd-status-') as directory:
    temp = Path(directory)
    (temp / 'esp_err.h').write_text('''#pragma once
 typedef int esp_err_t;
 #define ESP_OK 0
 #define ESP_FAIL -1
 #define ESP_ERR_INVALID_ARG 0x102
 #define ESP_ERR_INVALID_STATE 0x103
 #define ESP_ERR_NOT_SUPPORTED 0x106
 #define ESP_ERR_TIMEOUT 0x107
 #define ESP_ERR_INVALID_RESPONSE 0x108
''')
    (temp / 'test.c').write_text(shim + '\n'.join(functions) + tests)
    for poll_count in (2, 8, 32):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        f'-DCONFIG_ESP_SWD_HALT_POLL_COUNT={poll_count}',
                        '-I' + directory, '-I' + str(ROOT / 'interface'),
                        '-I' + str(ROOT / 'cmsis_dap'), str(temp / 'test.c'),
                        '-o', str(temp / 'test')], check=True)
        print(f'Halt poll count: {poll_count}', flush=True)
        subprocess.run([str(temp / 'test')], check=True)
