#!/usr/bin/env python3
"""Compile actual SWD initialization functions with scripted hardware failures."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'interface/swd_host.c').read_text()
functions = []
for name in ('swd_init', 'swd_init_debug'):
    match = re.search(r'^esp_err_t ' + name + r'\([^\n]*\)\n\{', source, re.M)
    end = source.index('\n}', match.end()) + 2
    functions.append(source[match.start():end])

shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
static uint32_t esp_cpu_get_cycle_count(void) { return 0; }
#include "swd_host.h"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define CONFIG_ESP_SWD_PHY_AXC2T245 1
#define pdMS_TO_TICKS(ms) (ms)
#define GPIO_INTR_DISABLE 0
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
typedef struct { int intr_type, mode, pull_down_en, pull_up_en; uint64_t pin_bit_mask; } gpio_config_t;
static struct { uint32_t select, csw; } dap_state;
static esp_err_t lock_error, port_error, config_error, level_error;
static esp_err_t attempt_error, final_error;
static unsigned attempts, stage, fail_stage, fail_attempts;
static unsigned off_calls, setup_calls, port_calls, gpio_calls, reads, delays, aborts;
static bool owned, never_ready;
static esp_err_t swd_begin_session(uint32_t ticks) {
    (void)ticks;
    if (!owned && lock_error != ESP_OK) return lock_error;
    owned = true;
    return ESP_OK;
}
static esp_err_t swd_esp_port_init(void) { ++port_calls; return port_error; }
void DAP_Setup(void) { ++setup_calls; }
static void PORT_SWD_SETUP(void) { assert(owned); }
esp_err_t swd_off(void) {
    assert(owned);
    owned = false;
    ++off_calls;
    return ESP_FAIL; // Cleanup status must never replace the original failure.
}
static esp_err_t __attribute__((unused)) gpio_config(const gpio_config_t *cfg) {
    (void)cfg; ++gpio_calls; return config_error;
}
static esp_err_t __attribute__((unused)) gpio_set_level(int pin, int level) {
    (void)pin; (void)level; ++gpio_calls; return level_error;
}
static void vTaskDelay(unsigned ticks) { assert(ticks == 20); ++delays; }
static void PIN_nRESET_OUT(int level) { (void)level; }
static esp_err_t next_stage(void) {
    ++stage;
    if (stage == fail_stage && attempts <= fail_attempts)
        return attempts == 4 ? final_error : attempt_error;
    return ESP_OK;
}
esp_err_t JTAG2SWD(void) { ++attempts; stage = 0; return next_stage(); }
esp_err_t swd_clear_errors(void) { return next_stage(); }
esp_err_t swd_write_dp(uint8_t adr, uint32_t val) {
    if (adr == DP_ABORT && val == DAPABORT) {
        ++aborts;
        return ESP_ERR_INVALID_ARG; // Failed best-effort recovery is not the root cause.
    }
    return next_stage();
}
esp_err_t swd_read_dp(uint8_t adr, uint32_t *val) {
    assert(adr == DP_CTRL_STAT);
    ++reads;
    *val = never_ready ? 0 : CDBGPWRUPACK | CSYSPWRUPACK;
    return next_stage();
}
static void reset(void) {
    lock_error = port_error = config_error = level_error = ESP_OK;
    attempt_error = final_error = ESP_FAIL;
    attempts = stage = fail_stage = 0;
    fail_attempts = 4;
    off_calls = setup_calls = port_calls = gpio_calls = reads = delays = aborts = 0;
    owned = never_ready = false;
}
'''

tests = r'''
int main(void) {
    reset(); lock_error = ESP_ERR_TIMEOUT;
    assert(swd_init(0) == ESP_ERR_TIMEOUT);
    assert(swd_init_debug(0) == ESP_ERR_TIMEOUT);
    assert(!owned && off_calls == 0 && port_calls == 0 && gpio_calls == 0);

    const esp_err_t errors[] = {ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reset(); port_error = errors[i];
        assert(swd_init(0) == errors[i]);
        assert(!owned && off_calls == 1 && setup_calls == 0);
        reset(); port_error = errors[i];
        assert(swd_init_debug(0) == errors[i]);
        assert(!owned && off_calls == 1 && attempts == 0);
    }

#if CONFIG_ESP_SWD_BOOT_PIN != -1
    reset(); config_error = ESP_ERR_INVALID_ARG;
    assert(swd_init_debug(0) == ESP_ERR_INVALID_ARG);
    assert(!owned && off_calls == 1 && attempts == 0 && gpio_calls == 1);
    reset(); level_error = ESP_ERR_INVALID_STATE;
    assert(swd_init_debug(0) == ESP_ERR_INVALID_STATE);
    assert(!owned && off_calls == 1 && attempts == 0 && gpio_calls == 2);
#endif

    // Every connection stage preserves its error through retries and cleanup.
    const esp_err_t wire_errors[] = {ESP_FAIL, ESP_ERR_INVALID_RESPONSE, ESP_ERR_TIMEOUT};
    for (unsigned s = 1; s <= 7; ++s) {
        for (unsigned i = 0; i < sizeof(wire_errors) / sizeof(wire_errors[0]); ++i) {
            reset(); fail_stage = s;
            attempt_error = final_error = wire_errors[i];
            assert(swd_init_debug(0) == wire_errors[i]);
            assert(!owned && off_calls == 1 && attempts == 4 && aborts == 3);
        }
        reset(); fail_stage = s; final_error = ESP_ERR_INVALID_RESPONSE;
        assert(swd_init_debug(0) == ESP_ERR_INVALID_RESPONSE); // last attempt
        reset(); fail_stage = s; fail_attempts = 3;
        assert(swd_init_debug(0) == ESP_OK); // recovery still succeeds
        assert(owned && off_calls == 0 && attempts == 4 && aborts == 3);
    }

    reset(); never_ready = true;
    assert(swd_init_debug(0) == ESP_ERR_TIMEOUT);
    assert(!owned && off_calls == 1 && attempts == 4 && reads == 400);

    reset();
    assert(swd_init_debug(0) == ESP_OK);
    assert(owned && off_calls == 0 && attempts == 1 && stage == 7 && reads == 1);
    assert(aborts == 0 && setup_calls == 1 && port_calls == 1);
    assert(delays == (CONFIG_ESP_SWD_BOOT_PIN == -1 ? 0 : 1));
    port_error = ESP_ERR_NO_MEM;
    assert(swd_init_debug(0) == ESP_ERR_NO_MEM); // failed re-init releases ownership
    assert(!owned && off_calls == 1);
    puts("PASS: setup/GPIO errors, connection-stage failures, retry recovery, power-up timeout and cleanup");
}
'''

with tempfile.TemporaryDirectory(prefix='swd-init-') as directory:
    temp = Path(directory)
    (temp / 'esp_err.h').write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
''')
    (temp / 'test.c').write_text(shim + '\n'.join(functions) + tests)
    for boot_pin in (-1, 2):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        f'-DCONFIG_ESP_SWD_BOOT_PIN={boot_pin}',
                        '-I' + directory, '-I' + str(ROOT / 'interface'),
                        '-I' + str(ROOT / 'cmsis_dap'), str(temp / 'test.c'),
                        '-o', str(temp / 'test')], check=True)
        print(f'BOOT pin: {boot_pin}', flush=True)
        subprocess.run([str(temp / 'test')], check=True)
