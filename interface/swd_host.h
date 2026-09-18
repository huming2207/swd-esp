/**
 * @file    swd_host.h
 * @brief   Host driver for accessing the DAP
 *
 * DAPLink Interface Firmware
 * Copyright (c) 2009-2019, ARM Limited, All Rights Reserved
 * Copyright 2019, Cypress Semiconductor Corporation 
 * or a subsidiary of Cypress Semiconductor Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <esp_err.h>

#include "debug_cm.h"
#include "swd_perf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONNECT_NORMAL,
    CONNECT_UNDER_RESET,
} SWD_CONNECT_TYPE;

typedef enum {
    FLASHALGO_RETURN_BOOL,
    FLASHALGO_RETURN_POINTER,
    FLASHALGO_RETURN_VALUE,
} flash_algo_return_t;

typedef struct __attribute__((__packed__)) {
    uint32_t breakpoint;
    uint32_t static_base;
    uint32_t stack_pointer;
} program_syscall_t;

/*
 * Start or reinitialize an SWD session, reserving the bus until swd_off().
 * Task context only. All operations and swd_off() must run in the owning task.
 * Repeated init calls by that task do not nest: one swd_off() ends the session.
 * The statically allocated mutex is created at startup and never deleted.
 *
 * ticks_to_wait is the lock timeout in FreeRTOS ticks (portMAX_DELAY waits
 * forever); it does not limit target connection retries. Both functions return
 * ESP_ERR_TIMEOUT if another task retains the bus, ESP_ERR_INVALID_STATE on
 * setup failure (or exhausted debug connection retries), and ESP_OK on success.
 * Setup/connection failure ends the session and releases the lock, including
 * on reinitialization. Lock timeout leaves the other task's session untouched.
 * swd_init() sets up the transport; swd_init_debug() also connects to the DAP.
 */
esp_err_t swd_init(uint32_t ticks_to_wait);
esp_err_t swd_init_debug(uint32_t ticks_to_wait);

/* Disconnect pins before releasing ownership. Returns 1 for an ended session,
 * or 0 without touching hardware if the calling task does not own a session. */
uint8_t swd_off(void);

/* Read/write and other target operations require an owned session. They do
 * not acquire locks internally, keeping the transfer path free of lock calls. */
uint8_t swd_clear_errors(void);
uint8_t swd_read_dp(uint8_t adr, uint32_t *val);
uint8_t swd_write_dp(uint8_t adr, uint32_t val);
uint8_t swd_read_ap(uint32_t adr, uint32_t *val);
uint8_t swd_write_ap(uint32_t adr, uint32_t val);
uint8_t swd_read_word(uint32_t addr, uint32_t *val);
uint8_t swd_write_word(uint32_t addr, uint32_t val);
uint8_t swd_read_byte(uint32_t addr, uint8_t *val);
uint8_t swd_write_byte(uint32_t addr, uint8_t val);
uint8_t swd_read_memory(uint32_t address, uint8_t *data, uint32_t size);
uint8_t swd_write_memory(uint32_t address, uint8_t *data, uint32_t size);
uint8_t swd_read_core_register(uint32_t n, uint32_t *val);
uint8_t swd_write_core_register(uint32_t n, uint32_t val);
uint8_t swd_flash_syscall_exec(const program_syscall_t *sys_call, uint32_t entry, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, flash_algo_return_t return_type, uint32_t *ret_out);
uint8_t swd_flash_syscall_exec_async(const program_syscall_t *sys_call, uint32_t entry, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4);
uint8_t swd_flash_syscall_wait_result(flash_algo_return_t return_type, uint32_t *ret_out);
uint8_t swd_transfer_retry(uint32_t req, uint32_t *data);
uint8_t swd_halt_target();
uint8_t swd_wait_until_halted(void);
void int2array(uint8_t *res, uint32_t data, uint8_t len);
void swd_set_reset_connect(SWD_CONNECT_TYPE type);
void swd_set_soft_reset(uint32_t soft_reset_type);
uint8_t swd_read_idcode(uint32_t *id);
void swd_trigger_nrst();
uint8_t JTAG2SWD(void);

#ifdef __cplusplus
}
#endif
