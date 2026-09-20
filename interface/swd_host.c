/**
 * @file    swd_host.c
 * @brief   Implementation of swd_host.h
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

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <esp_timer.h>

#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
#include <esp_cpu.h>
#include <esp_err.h>
#if defined(CONFIG_ESP_SWD_USE_DEDICATED_GPIO)
#include <driver/dedic_gpio.h>
#endif
#endif

#include "swd_host.h"
#include "debug_cm.h"
#include "DAP_config.h"
#include "DAP.h"


#include <esp_log.h>
#define DAP_TAG "swd"


#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
uint32_t g_swd_dedic_clk_mask;
uint32_t g_swd_dedic_data_out_mask;
uint32_t g_swd_dedic_data_in_mask;
#ifdef CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS
uint32_t g_swd_dedic_translator_noe_mask;
uint32_t g_swd_dedic_translator_dir1_mask;
uint32_t g_swd_dedic_translator_dir_mask;
#endif

static bool swd_port_initialized;

#ifdef CONFIG_ESP_SWD_USE_DEDICATED_GPIO
static dedic_gpio_bundle_handle_t swd_out_bundle;
#ifndef CONFIG_ESP_SWD_USE_SPI
static dedic_gpio_bundle_handle_t swd_in_bundle;
#endif
#endif

esp_err_t swd_esp_port_init(void)
{
    if (swd_port_initialized) {
        return ESP_OK;
    }

    /* Establish safe latch values before enabling any ESP32 output drivers. */
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_DATA_NOE_PIN, 1U);
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_CLK_NOE_PIN, 1U);
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_CLK_PIN, 0U);
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN, 1U);
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_DATA_DIR1_PIN, 1U);
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_DATA_DIR2_PIN, 0U);
#if CONFIG_ESP_SWD_BOOT_PIN >= 0
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_BOOT_PIN, 0U);
#endif
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_NRST_PIN, 0U);

    uint64_t output_mask =
        (1ULL << CONFIG_ESP_SWD_DATA_NOE_PIN) |
        (1ULL << CONFIG_ESP_SWD_CLK_NOE_PIN) |
        (1ULL << CONFIG_ESP_SWD_CLK_PIN) |
        (1ULL << CONFIG_ESP_SWD_DATA_OUT_PIN) |
        (1ULL << CONFIG_ESP_SWD_DATA_DIR1_PIN) |
        (1ULL << CONFIG_ESP_SWD_DATA_DIR2_PIN) |
        (1ULL << CONFIG_ESP_SWD_NRST_PIN);
#if CONFIG_ESP_SWD_BOOT_PIN >= 0
    output_mask |= 1ULL << CONFIG_ESP_SWD_BOOT_PIN;
#endif

    const gpio_config_t output_config = {
        .pin_bit_mask = output_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&output_config);
    if (ret != ESP_OK) {
        return ret;
    }

    const gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << CONFIG_ESP_SWD_DATA_IN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&input_config);
    if (ret != ESP_OK) {
        return ret;
    }

#ifdef CONFIG_ESP_SWD_USE_DEDICATED_GPIO
    /* A dedicated-GPIO bundle belongs to the CPU which creates it. */
    const BaseType_t task_core = xTaskGetCoreID(NULL);
    if (task_core == tskNO_AFFINITY) {
        ESP_LOGE(DAP_TAG, "Dedicated GPIO requires the calling task to be pinned");
        return ESP_ERR_INVALID_STATE;
    }
    const int core_id = esp_cpu_get_core_id();
    if (task_core != core_id) {
        ESP_LOGE(DAP_TAG, "SWD task affinity does not match its current CPU");
        return ESP_ERR_INVALID_STATE;
    }

#ifdef CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS
    /* External pulls keep the translator isolated while its pins are rerouted. */
#ifndef CONFIG_ESP_SWD_USE_SPI
    gpio_ll_output_disable(&GPIO, CONFIG_ESP_SWD_DATA_NOE_PIN);
#endif
    gpio_ll_output_disable(&GPIO, CONFIG_ESP_SWD_DATA_DIR1_PIN);
    gpio_ll_output_disable(&GPIO, CONFIG_ESP_SWD_DATA_DIR2_PIN);
#endif

    const int out_gpios[] = {
#ifdef CONFIG_ESP_SWD_USE_SPI
        CONFIG_ESP_SWD_DATA_DIR1_PIN,
        CONFIG_ESP_SWD_DATA_DIR2_PIN,
#else
        CONFIG_ESP_SWD_CLK_PIN,
        CONFIG_ESP_SWD_DATA_OUT_PIN,
#ifdef CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS
        CONFIG_ESP_SWD_DATA_NOE_PIN,
        CONFIG_ESP_SWD_DATA_DIR1_PIN,
        CONFIG_ESP_SWD_DATA_DIR2_PIN,
#endif
#endif
    };
    const dedic_gpio_bundle_config_t out_config = {
        .gpio_array = out_gpios,
        .array_size = sizeof(out_gpios) / sizeof(out_gpios[0]),
        .flags = {
            .out_en = 1,
        },
    };

    ret = dedic_gpio_new_bundle(&out_config, &swd_out_bundle);
    if (ret != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to allocate SWD output bundle: %s", esp_err_to_name(ret));
        return ret;
    }

    uint32_t out_offset;
    ret = dedic_gpio_get_out_offset(swd_out_bundle, &out_offset);
    if (ret != ESP_OK) {
        dedic_gpio_del_bundle(swd_out_bundle);
        swd_out_bundle = NULL;
        return ret;
    }
#ifdef CONFIG_ESP_SWD_USE_SPI
    g_swd_dedic_translator_dir1_mask = 1U << out_offset;
    g_swd_dedic_translator_dir_mask = 0x3U << out_offset;

    /* The hardware CS output keeps /OE high; preload DIR1 high and DIR2 low. */
    dedic_gpio_bundle_write(swd_out_bundle, 0x3U, 0x1U);
    gpio_ll_output_enable(&GPIO, CONFIG_ESP_SWD_DATA_DIR1_PIN);
    gpio_ll_output_enable(&GPIO, CONFIG_ESP_SWD_DATA_DIR2_PIN);
#else
    g_swd_dedic_clk_mask = 1U << out_offset;
    g_swd_dedic_data_out_mask = 1U << (out_offset + 1U);
#ifdef CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS
    g_swd_dedic_translator_noe_mask = 1U << (out_offset + 2U);
    g_swd_dedic_translator_dir1_mask = 1U << (out_offset + 3U);
    g_swd_dedic_translator_dir_mask = 0x3U << (out_offset + 3U);

    /* SWCLK low, SWDIO high, translator isolated, DIR1 high, DIR2 low. */
    dedic_gpio_bundle_write(swd_out_bundle, 0x1FU, 0x0EU);
    gpio_ll_output_enable(&GPIO, CONFIG_ESP_SWD_DATA_DIR1_PIN);
    gpio_ll_output_enable(&GPIO, CONFIG_ESP_SWD_DATA_DIR2_PIN);
    gpio_ll_output_enable(&GPIO, CONFIG_ESP_SWD_DATA_NOE_PIN);
#else
    dedic_gpio_bundle_write(swd_out_bundle, 0x3U, 0x2U);
#endif
#endif

#ifndef CONFIG_ESP_SWD_USE_SPI
    const int in_gpios[] = {
        CONFIG_ESP_SWD_DATA_IN_PIN,
    };
    const dedic_gpio_bundle_config_t in_config = {
        .gpio_array = in_gpios,
        .array_size = sizeof(in_gpios) / sizeof(in_gpios[0]),
        .flags = {
            .in_en = 1,
        },
    };

    ret = dedic_gpio_new_bundle(&in_config, &swd_in_bundle);
    if (ret != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to allocate SWD input bundle: %s", esp_err_to_name(ret));
        dedic_gpio_del_bundle(swd_out_bundle);
        swd_out_bundle = NULL;
        g_swd_dedic_clk_mask = 0U;
        g_swd_dedic_data_out_mask = 0U;
#ifdef CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS
        g_swd_dedic_translator_noe_mask = 0U;
        g_swd_dedic_translator_dir1_mask = 0U;
        g_swd_dedic_translator_dir_mask = 0U;
#endif
        return ret;
    }

    uint32_t in_offset;
    ret = dedic_gpio_get_in_offset(swd_in_bundle, &in_offset);
    if (ret != ESP_OK) {
        dedic_gpio_del_bundle(swd_in_bundle);
        dedic_gpio_del_bundle(swd_out_bundle);
        swd_in_bundle = NULL;
        swd_out_bundle = NULL;
        g_swd_dedic_clk_mask = 0U;
        g_swd_dedic_data_out_mask = 0U;
#ifdef CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS
        g_swd_dedic_translator_noe_mask = 0U;
        g_swd_dedic_translator_dir1_mask = 0U;
        g_swd_dedic_translator_dir_mask = 0U;
#endif
        return ret;
    }
    g_swd_dedic_data_in_mask = 1U << in_offset;
#else
    ret = swd_esp_spi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to initialize direct SPI2 SWD: %s",
                 esp_err_to_name(ret));
        dedic_gpio_del_bundle(swd_out_bundle);
        swd_out_bundle = NULL;
        g_swd_dedic_translator_dir1_mask = 0U;
        g_swd_dedic_translator_dir_mask = 0U;
        return ret;
    }
#endif

#endif

#ifdef CONFIG_ESP_SWD_USE_PARLIO
    ret = swd_esp_parlio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to initialize PARLIO SWD: %s",
                 esp_err_to_name(ret));
        return ret;
    }
#endif

    swd_port_initialized = true;
#if CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ == -1 && !defined(CONFIG_ESP_SWD_USE_SPI) && !defined(CONFIG_ESP_SWD_USE_PARLIO)
    ESP_LOGW(DAP_TAG, "Fast SWD mode enabled: %u NOPs per half-cycle",
             CONFIG_ESP_SWD_FAST_DELAY_NOPS);
#endif
#ifdef CONFIG_ESP_SWD_USE_SPI
    ESP_LOGI(DAP_TAG,
             "Translated SWD initialized with direct SPI2, "
             "requested=%u Hz, actual=%u Hz, hardware-CS translator enable, "
             "turnaround guard=%u ns, CS setup=%u clocks, RX alignment=%s",
             CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ,
             swd_esp_spi_actual_clock_hz(), ESP_SWD_TURNAROUND_GUARD_NS,
             swd_esp_spi_cs_setup_cycles(),
             swd_esp_spi_rx_standard_alignment() ? "standard" : "delayed");
#elif defined(CONFIG_ESP_SWD_USE_PARLIO)
    ESP_LOGI(DAP_TAG,
             "Rev 7.1 SWD initialized with direct PARLIO LL/GDMA, "
             "requested=%u Hz, actual=%u Hz, turnaround guard=%u ns",
             CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ,
             swd_esp_parlio_actual_clock_hz(), ESP_SWD_TURNAROUND_GUARD_NS);
#else
    ESP_LOGI(DAP_TAG, "Rev 6 SWD GPIO initialized%s, turnaround guard=%u ns",
#ifdef CONFIG_ESP_SWD_USE_DEDICATED_GPIO
             " with dedicated GPIO",
#else
             "",
#endif
             ESP_SWD_TURNAROUND_GUARD_NS
    );
#endif
    return ESP_OK;
}

void swd_esp_port_setup(void)
{
    if (!swd_port_initialized) {
        return;
    }

#ifdef CONFIG_ESP_SWD_USE_SPI
    swd_esp_spi_setup();
#elif defined(CONFIG_ESP_SWD_USE_PARLIO)
    swd_esp_parlio_setup();
#endif
    PIN_SWCLK_TCK_CLR();
#if CONFIG_ESP_SWD_BOOT_PIN >= 0
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_BOOT_PIN, 0U);
#endif
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_NRST_PIN, 0U);
#ifndef CONFIG_ESP_SWD_USE_PARLIO
    swd_esp_swdio_host_drive(1U);
#endif
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_CLK_NOE_PIN, 0U);
}

void swd_esp_port_off(void)
{
    if (!swd_port_initialized) {
        return;
    }

    PIN_SWCLK_TCK_CLR();
#ifdef CONFIG_ESP_SWD_USE_PARLIO
    swd_esp_parlio_off();
#elif defined(CONFIG_ESP_SWD_USE_SPI)
    swd_esp_spi_off();
    swd_esp_translator_set_noe(1U);
    gpio_ll_output_disable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
    gpio_ll_input_enable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
    swd_esp_translator_set_direction(0U);
#else
    swd_esp_translator_set_noe(1U);
    gpio_ll_output_disable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
    gpio_ll_input_enable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
    swd_esp_translator_set_direction(0U);
#endif
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_CLK_NOE_PIN, 1U);
#if CONFIG_ESP_SWD_BOOT_PIN >= 0
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_BOOT_PIN, 0U);
#endif
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_NRST_PIN, 0U);
}
#endif


// Probably not 1024
#ifndef TARGET_AUTO_INCREMENT_PAGE_SIZE
#define TARGET_AUTO_INCREMENT_PAGE_SIZE (1024)
#endif

// Default NVIC and Core debug base addresses
// TODO: Read these addresses from ROM.
#define NVIC_Addr    (0xe000e000)
#define DBG_Addr     (0xe000edf0)

// AP CSW register, base value
#define CSW_VALUE (CSW_RESERVED | CSW_MSTRDBG | CSW_HPROT | CSW_DBGSTAT | CSW_SADDRINC)

#define DCRDR 0xE000EDF8
#define DCRSR 0xE000EDF4
#define DHCSR 0xE000EDF0
#define REGWnR (1 << 16)

#define MAX_SWD_RETRY 100//10

#define MAX_TIMEOUT   UINT32_MAX  // Timeout for syscalls on target

// Use the CMSIS-Core definition if available.
#if !defined(SCB_AIRCR_PRIGROUP_Pos)
#define SCB_AIRCR_PRIGROUP_Pos              8U                                            /*!< SCB AIRCR: PRIGROUP Position */
#define SCB_AIRCR_PRIGROUP_Msk             (7UL << SCB_AIRCR_PRIGROUP_Pos)                /*!< SCB AIRCR: PRIGROUP Mask */
#endif

typedef struct {
    uint32_t select;
    uint32_t csw;
} DAP_STATE;

typedef struct {
    uint32_t r[16];
    uint32_t xpsr;
} DEBUG_STATE;

static SWD_CONNECT_TYPE reset_connect = CONNECT_NORMAL;

static DAP_STATE dap_state;
static uint32_t  soft_reset = SYSRESETREQ;

static uint32_t swd_get_apsel(uint32_t adr)
{
    uint32_t apsel = 0; // target_get_apsel();
    if (!apsel)
        return adr & 0xff000000;
    else
        return apsel;
}

void swd_set_reset_connect(SWD_CONNECT_TYPE type)
{
    reset_connect = type;
}

void IRAM_ATTR int2array(uint8_t *res, uint32_t data, uint8_t len)
{
    uint8_t i = 0;

    for (i = 0; i < len; i++) {
        res[i] = (data >> 8 * i) & 0xff;
    }
}

esp_err_t IRAM_ATTR swd_transfer_retry(uint32_t req, uint32_t *data)
{
    uint8_t ack = DAP_TRANSFER_WAIT;
    uint32_t wait_count = 0;

    for (uint32_t i = 0; i < MAX_SWD_RETRY; i++) {
        ack = SWD_Transfer(req, data);

        // if ack != WAIT
        if (ack != DAP_TRANSFER_WAIT) {
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
            swd_perf_record_retry(req, wait_count, ack);
#endif
            switch (ack) {
            case DAP_TRANSFER_OK:
                return ESP_OK;
            case DAP_TRANSFER_FAULT:
                return ESP_FAIL;
            default:
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        wait_count++;
    }

#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
    swd_perf_record_retry(req, wait_count, ack);
#endif
    return ESP_ERR_TIMEOUT;
}

void swd_set_soft_reset(uint32_t soft_reset_type)
{
    soft_reset = soft_reset_type;
}

/* ESP-IDF runs constructors before starting application tasks. Create the
 * permanent mutex there so first-use races need no FreeRTOS calls inside a
 * portMUX critical section. */
static StaticSemaphore_t swd_lock_storage;
static SemaphoreHandle_t swd_lock;

static void __attribute__((constructor)) swd_create_lock(void)
{
    swd_lock = xSemaphoreCreateMutexStatic(&swd_lock_storage);
}

static esp_err_t swd_begin_session(uint32_t ticks_to_wait)
{
    SemaphoreHandle_t lock = swd_lock;
    // Reinitialization by the owner belongs to the same session: one off()
    // releases it, regardless of how many init calls were made.
    if (xSemaphoreGetMutexHolder(lock) == xTaskGetCurrentTaskHandle()) {
        return ESP_OK;
    }
    return xSemaphoreTake(lock, ticks_to_wait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t swd_init(uint32_t ticks_to_wait)
{
    esp_err_t err = swd_begin_session(ticks_to_wait);
    if (err != ESP_OK) {
        return err;
    }

#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    if ((err = swd_esp_port_init()) != ESP_OK) {
        swd_off();
        return err;
    }
#endif
    //TODO - DAP_Setup puts GPIO pins in a hi-z state which can
    //       cause problems on re-init.  This needs to be investigated
    //       and fixed.
    DAP_Setup();
    PORT_SWD_SETUP();
    return ESP_OK;
}

esp_err_t swd_off(void)
{
    SemaphoreHandle_t lock = swd_lock;
    if (xSemaphoreGetMutexHolder(lock) != xTaskGetCurrentTaskHandle()) {
        // A task without a session must not touch another task's pins.
        return ESP_ERR_INVALID_STATE;
    }

#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    PORT_OFF();
#else
    gpio_set_level(CONFIG_ESP_SWD_BOOT_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    PIN_nRESET_OUT(0);
    vTaskDelay(pdMS_TO_TICKS(350));
    PIN_nRESET_OUT(1);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_reset_pin(CONFIG_ESP_SWD_BOOT_PIN);
    gpio_reset_pin(PIN_SWCLK);
    gpio_reset_pin(PIN_SWDIO);
    gpio_reset_pin(PIN_nRST);
#endif
    (void)xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_clear_errors(void)
{
    esp_err_t err;
    if ((err = swd_write_dp(DP_ABORT, STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR)) != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

// Read debug port register.
esp_err_t IRAM_ATTR swd_read_dp(uint8_t adr, uint32_t *val)
{
    if (val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    uint32_t tmp_in;
    uint8_t tmp_out[4];
    uint32_t tmp;
    tmp_in = SWD_REG_DP | SWD_REG_R | SWD_REG_ADR(adr);
    err = swd_transfer_retry(tmp_in, (uint32_t *)tmp_out);
    if (err != ESP_OK) {
        return err;
    }
    *val = 0;
    tmp = tmp_out[3];
    *val |= (tmp << 24);
    tmp = tmp_out[2];
    *val |= (tmp << 16);
    tmp = tmp_out[1];
    *val |= (tmp << 8);
    tmp = tmp_out[0];
    *val |= (tmp << 0);
    return err;
}

// Write debug port register
esp_err_t IRAM_ATTR swd_write_dp(uint8_t adr, uint32_t val)
{
    esp_err_t err;
    uint32_t req;
    uint8_t data[4];

    //check if the right bank is already selected
    if ((adr == DP_SELECT) && (dap_state.select == val)) {
        return ESP_OK;
    }

    req = SWD_REG_DP | SWD_REG_W | SWD_REG_ADR(adr);
    int2array(data, val, 4);
    err = swd_transfer_retry(req, (uint32_t *)data);
    if ((err == ESP_OK) && (adr == DP_SELECT)) {
        dap_state.select = val;
    }

    return err;
}

// Read access port register.
esp_err_t IRAM_ATTR swd_read_ap(uint32_t adr, uint32_t *val)
{
    if (val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    uint8_t tmp_in;
    uint8_t tmp_out[4];
    uint32_t tmp;
    uint32_t apsel = swd_get_apsel(adr);
    uint32_t bank_sel = adr & APBANKSEL;

    if ((err = swd_write_dp(DP_SELECT, apsel | bank_sel)) != ESP_OK) {
        return err;
    }

    tmp_in = SWD_REG_AP | SWD_REG_R | SWD_REG_ADR(adr);
    // first dummy read
    err = swd_transfer_retry(tmp_in, (uint32_t *)tmp_out);
    if (err != ESP_OK) {
        return err;
    }
    err = swd_transfer_retry(tmp_in, (uint32_t *)tmp_out);
    if (err != ESP_OK) {
        return err;
    }
    *val = 0;
    tmp = tmp_out[3];
    *val |= (tmp << 24);
    tmp = tmp_out[2];
    *val |= (tmp << 16);
    tmp = tmp_out[1];
    *val |= (tmp << 8);
    tmp = tmp_out[0];
    *val |= (tmp << 0);
    return err;
}

// Write access port register
esp_err_t IRAM_ATTR swd_write_ap(uint32_t adr, uint32_t val)
{
    esp_err_t err;
    uint8_t data[4];
    uint8_t req;
    uint32_t apsel = swd_get_apsel(adr);
    uint32_t bank_sel = adr & APBANKSEL;

    if ((err = swd_write_dp(DP_SELECT, apsel | bank_sel)) != ESP_OK) {
        return err;
    }

    switch (adr) {
        case AP_CSW:
            if (dap_state.csw == val) {
                return ESP_OK;
            }

            break;

        default:
            break;
    }

    req = SWD_REG_AP | SWD_REG_W | SWD_REG_ADR(adr);
    int2array(data, val, 4);

    if ((err = swd_transfer_retry(req, (uint32_t *)data)) != ESP_OK) {
        return err;
    }

    req = SWD_REG_DP | SWD_REG_R | SWD_REG_ADR(DP_RDBUFF);
    err = swd_transfer_retry(req, NULL);
    if (err == ESP_OK && adr == AP_CSW) {
        dap_state.csw = val;
    }
    return err;
}


// Write 32-bit word aligned values to target memory using address auto-increment.
// size is in bytes.
static IRAM_ATTR esp_err_t swd_write_block(uint32_t address, uint8_t *data, uint32_t size)
{
    esp_err_t err;
    uint8_t tmp_in[4], req;
    uint32_t size_in_words;
    uint32_t i;

    if (data == NULL || size == 0 || (size & 3) != 0 || (address & 3) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_in_words = size / 4;

    // CSW register
    if ((err = swd_write_ap(AP_CSW, CSW_VALUE | CSW_SIZE32)) != ESP_OK) {
        return err;
    }

    // TAR write
    req = SWD_REG_AP | SWD_REG_W | (1 << 2);
    int2array(tmp_in, address, 4);

    if ((err = swd_transfer_retry(req, (uint32_t *)tmp_in)) != ESP_OK) {
        return err;
    }

    // DRW write
    req = SWD_REG_AP | SWD_REG_W | (3 << 2);

    for (i = 0; i < size_in_words; i++) {
        if ((err = swd_transfer_retry(req, (uint32_t *)data)) != ESP_OK) {
            return err;
        }

        data += 4;
    }

    // dummy read
    req = SWD_REG_DP | SWD_REG_R | SWD_REG_ADR(DP_RDBUFF);
    err = swd_transfer_retry(req, NULL);
    return err;
}

// Read 32-bit word aligned values from target memory using address auto-increment.
// size is in bytes.
static esp_err_t IRAM_ATTR swd_read_block(uint32_t address, uint8_t *data, uint32_t size)
{
    esp_err_t err;
    uint8_t tmp_in[4], req;
    uint32_t size_in_words;
    uint32_t i;

    if (data == NULL || size == 0 || (size & 3) != 0 || (address & 3) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_in_words = size / 4;

    if ((err = swd_write_ap(AP_CSW, CSW_VALUE | CSW_SIZE32)) != ESP_OK) {
        return err;
    }

    // TAR write
    req = SWD_REG_AP | SWD_REG_W | AP_TAR;
    int2array(tmp_in, address, 4);

    if ((err = swd_transfer_retry(req, (uint32_t *)tmp_in)) != ESP_OK) {
        return err;
    }

    // read data
    req = SWD_REG_AP | SWD_REG_R | AP_DRW;

    // initiate first read, data comes back in next read
    if ((err = swd_transfer_retry(req, NULL)) != ESP_OK) {
        return err;
    }

    for (i = 0; i < (size_in_words - 1); i++) {
        if ((err = swd_transfer_retry(req, (uint32_t *)data)) != ESP_OK) {
            return err;
        }

        data += 4;
    }

    // read last word
    req = SWD_REG_DP | SWD_REG_R | SWD_REG_ADR(DP_RDBUFF);
    err = swd_transfer_retry(req, (uint32_t *)data);
    return err;
}

// Read target memory.
static esp_err_t IRAM_ATTR swd_read_data(uint32_t addr, uint32_t *val)
{
    esp_err_t err;
    uint8_t tmp_in[4];
    uint8_t tmp_out[4];
    uint8_t req;
    uint32_t tmp;
    // put addr in TAR register
    int2array(tmp_in, addr, 4);
    req = SWD_REG_AP | SWD_REG_W | (1 << 2);

    if ((err = swd_transfer_retry(req, (uint32_t *)tmp_in)) != ESP_OK) {
        return err;
    }

    // read data
    req = SWD_REG_AP | SWD_REG_R | (3 << 2);

    if ((err = swd_transfer_retry(req, (uint32_t *)tmp_out)) != ESP_OK) {
        return err;
    }

    // dummy read
    req = SWD_REG_DP | SWD_REG_R | SWD_REG_ADR(DP_RDBUFF);
    err = swd_transfer_retry(req, (uint32_t *)tmp_out);
    if (err != ESP_OK) {
        return err;
    }
    *val = 0;
    tmp = tmp_out[3];
    *val |= (tmp << 24);
    tmp = tmp_out[2];
    *val |= (tmp << 16);
    tmp = tmp_out[1];
    *val |= (tmp << 8);
    tmp = tmp_out[0];
    *val |= (tmp << 0);
    return err;
}

// Write target memory.
static esp_err_t IRAM_ATTR swd_write_data(uint32_t address, uint32_t data)
{
    esp_err_t err;
    uint8_t tmp_in[4];
    uint8_t req;
    // put addr in TAR register
    int2array(tmp_in, address, 4);
    req = SWD_REG_AP | SWD_REG_W | (1 << 2);

    if ((err = swd_transfer_retry(req, (uint32_t *)tmp_in)) != ESP_OK) {
        return err;
    }

    // write data
    int2array(tmp_in, data, 4);
    req = SWD_REG_AP | SWD_REG_W | (3 << 2);

    if ((err = swd_transfer_retry(req, (uint32_t *)tmp_in)) != ESP_OK) {
        return err;
    }

    // dummy read
    req = SWD_REG_DP | SWD_REG_R | SWD_REG_ADR(DP_RDBUFF);
    err = swd_transfer_retry(req, NULL);
    return err;
}

// Read 32-bit word from target memory.
esp_err_t IRAM_ATTR swd_read_word(uint32_t addr, uint32_t *val)
{
    if (val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    if ((err = swd_write_ap(AP_CSW, CSW_VALUE | CSW_SIZE32)) != ESP_OK) {
        return err;
    }

    if ((err = swd_read_data(addr, val)) != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

// Write 32-bit word to target memory.
esp_err_t IRAM_ATTR swd_write_word(uint32_t addr, uint32_t val)
{
    esp_err_t err;
    if ((err = swd_write_ap(AP_CSW, CSW_VALUE | CSW_SIZE32)) != ESP_OK) {
        return err;
    }

    if ((err = swd_write_data(addr, val)) != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

// Read 8-bit byte from target memory.
esp_err_t IRAM_ATTR swd_read_byte(uint32_t addr, uint8_t *val)
{
    if (val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    uint32_t tmp;

    if ((err = swd_write_ap(AP_CSW, CSW_VALUE | CSW_SIZE8)) != ESP_OK) {
        return err;
    }

    if ((err = swd_read_data(addr, &tmp)) != ESP_OK) {
        return err;
    }

    *val = (uint8_t)(tmp >> ((addr & 0x03) << 3));
    return ESP_OK;
}

// Write 8-bit byte to target memory.
esp_err_t IRAM_ATTR swd_write_byte(uint32_t addr, uint8_t val)
{
    esp_err_t err;
    uint32_t tmp;

    if ((err = swd_write_ap(AP_CSW, CSW_VALUE | CSW_SIZE8)) != ESP_OK) {
        return err;
    }

    tmp = val << ((addr & 0x03) << 3);

    if ((err = swd_write_data(addr, tmp)) != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

// Read unaligned data from target memory.
// size is in bytes.
esp_err_t IRAM_ATTR swd_read_memory(uint32_t address, uint8_t *data, uint32_t size)
{
    if (data == NULL && size != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    uint32_t n;

    // Read bytes until word aligned
    while ((size > 0) && (address & 0x3)) {
        if ((err = swd_read_byte(address, data)) != ESP_OK) {
            return err;
        }

        address++;
        data++;
        size--;
    }

    // Read word aligned blocks
    while (size > 3) {
        // Limit to auto increment page size
        n = TARGET_AUTO_INCREMENT_PAGE_SIZE - (address & (TARGET_AUTO_INCREMENT_PAGE_SIZE - 1));

        if (size < n) {
            n = size & 0xFFFFFFFC; // Only count complete words remaining
        }

        if ((err = swd_read_block(address, data, n)) != ESP_OK) {
            return err;
        }

        address += n;
        data += n;
        size -= n;
    }

    // Read remaining bytes
    while (size > 0) {
        if ((err = swd_read_byte(address, data)) != ESP_OK) {
            return err;
        }

        address++;
        data++;
        size--;
    }

    return ESP_OK;
}

// Write unaligned data to target memory.
// size is in bytes.
esp_err_t IRAM_ATTR swd_write_memory(uint32_t address, uint8_t *data, uint32_t size)
{
    if (data == NULL && size != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    uint32_t n = 0;

    // Write bytes until word aligned
    while ((size > 0) && (address & 0x3)) {
        if ((err = swd_write_byte(address, *data)) != ESP_OK) {
            return err;
        }

        address++;
        data++;
        size--;
    }

    // Write word aligned blocks
    while (size > 3) {
        // Limit to auto increment page size
        n = TARGET_AUTO_INCREMENT_PAGE_SIZE - (address & (TARGET_AUTO_INCREMENT_PAGE_SIZE - 1));

        if (size < n) {
            n = size & 0xFFFFFFFC; // Only count complete words remaining
        }

        if ((err = swd_write_block(address, data, n)) != ESP_OK) {
            return err;
        }

        address += n;
        data += n;
        size -= n;
    }

    // Write remaining bytes
    while (size > 0) {
        if ((err = swd_write_byte(address, *data)) != ESP_OK) {
            return err;
        }

        address++;
        data++;
        size--;
    }

    return ESP_OK;
}

// Execute system call.
static esp_err_t IRAM_ATTR swd_write_debug_state(DEBUG_STATE *state)
{
    esp_err_t err;
    uint32_t i, status;

    if ((err = swd_write_dp(DP_SELECT, 0)) != ESP_OK) {
        return err;
    }

    // R0, R1, R2, R3
    for (i = 0; i < 4; i++) {
        if ((err = swd_write_core_register(i, state->r[i])) != ESP_OK) {
            ESP_LOGE(DAP_TAG, "Failed to set R0-3");
            return err;
        }
    }

    // R9
    if ((err = swd_write_core_register(9, state->r[9])) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to set R9");
        return err;
    }

    // R13, R14, R15
    for (i = 13; i < 16; i++) {
        if ((err = swd_write_core_register(i, state->r[i])) != ESP_OK) {
            ESP_LOGE(DAP_TAG, "Failed to set R13-15");
            return err;
        }
    }

    // xPSR
    if ((err = swd_write_core_register(16, state->xpsr)) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to set xPSR");
        return err;
    }

    if ((err = swd_write_word(DBG_HCSR, DBGKEY | C_DEBUGEN | C_MASKINTS | C_HALT)) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to set halt");
        return err;
    }

    if ((err = swd_write_word(DBG_HCSR, DBGKEY | C_DEBUGEN | C_MASKINTS)) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to set unhalt");
        return err;
    }

    // check status
    if ((err = swd_read_dp(DP_CTRL_STAT, &status)) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to check status");
        return err;
    }

    if (status & (STICKYERR | WDATAERR)) {
        ESP_LOGE(DAP_TAG, "Status has error");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_read_core_register(uint32_t n, uint32_t *val)
{
    if (val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    uint32_t status;
    int i = 0, timeout = 100;

    if ((err = swd_write_word(DCRSR, n)) != ESP_OK) {
        return err;
    }

    // wait for S_REGRDY
    for (i = 0; i < timeout; i++) {
        if ((err = swd_read_word(DHCSR, &status)) != ESP_OK) {
            return err;
        }

        if (status & S_REGRDY) {
            break;
        }
    }

    if (i == timeout) {
        ESP_LOGE(DAP_TAG, "Timeout");
        return ESP_ERR_TIMEOUT;
    }

    if ((err = swd_read_word(DCRDR, val)) != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_write_core_register(uint32_t n, uint32_t val)
{
    esp_err_t err;
    int i = 0, timeout = 100;

    if ((err = swd_write_word(DCRDR, val)) != ESP_OK) {
        return err;
    }

    if ((err = swd_write_word(DCRSR, n | REGWnR)) != ESP_OK) {
        return err;
    }

    // wait for S_REGRDY
    for (i = 0; i < timeout; i++) {
        if ((err = swd_read_word(DHCSR, &val)) != ESP_OK) {
            return err;
        }

        if (val & S_REGRDY) {
            return ESP_OK;
        }
    }

    ESP_LOGE(DAP_TAG, "Core timeout");
    return ESP_ERR_TIMEOUT;
}

esp_err_t IRAM_ATTR swd_wait_until_halted(void)
{
    const int64_t deadline = esp_timer_get_time() + 5000000; // Five seconds.
    uint32_t polls = 0;

    while (esp_timer_get_time() < deadline) {
        uint32_t val;
        esp_err_t err = swd_read_word(DBG_HCSR, &val);
        if (err != ESP_OK) {
            return err;
        }
        if (val & S_HALT) {
            return ESP_OK;
        }

        if (++polls == CONFIG_ESP_SWD_HALT_POLL_COUNT) {
            // Let other tasks run while retaining ownership of the SWD session.
            vTaskDelay(1);
            polls = 0;
        }
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t IRAM_ATTR swd_flash_syscall_exec(const program_syscall_t *sys_call, uint32_t entry, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, flash_algo_return_t return_type, uint32_t *ret_out)
{
    if (sys_call == NULL || (return_type != FLASHALGO_RETURN_BOOL &&
        return_type != FLASHALGO_RETURN_POINTER && return_type != FLASHALGO_RETURN_VALUE)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    DEBUG_STATE state = {{0}, 0};
    // Call flash algorithm function on target and wait for result.
    state.r[0]     = arg1;                   // R0: Argument 1
    state.r[1]     = arg2;                   // R1: Argument 2
    state.r[2]     = arg3;                   // R2: Argument 3
    state.r[3]     = arg4;                   // R3: Argument 4
    state.r[9]     = sys_call->static_base;    // SB: Static Base
    state.r[13]    = sys_call->stack_pointer;  // SP: Stack Pointer
    state.r[14]    = sys_call->breakpoint;     // LR: Exit Point
    state.r[15]    = entry;                        // PC: Entry Point
    state.xpsr     = 0x01000000;          // xPSR: T = 1, ISR = 0

    if ((err = swd_write_debug_state(&state)) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to set state");
        return err;
    }

    if ((err = swd_wait_until_halted()) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to halt");
        return err;
    }

    if ((err = swd_read_core_register(0, &state.r[0])) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to read register");
        return err;
    }

    //remove the C_MASKINTS
    if ((err = swd_write_word(DBG_HCSR, DBGKEY | C_DEBUGEN | C_HALT)) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to halt again");
        return err;
    }

    if (return_type == FLASHALGO_RETURN_POINTER) {
        // Flash verify functions return pointer to byte following the buffer if successful.
        if (state.r[0] != (arg1 + arg2)) {
            return ESP_FAIL;
        }
    } else if (return_type == FLASHALGO_RETURN_VALUE) {
        if (ret_out != NULL) {
            *ret_out = state.r[0];
        }
    } else {
//         ESP_LOGW(DAP_TAG, "R0 = %d", state.r[0]);
//
//        uint32_t r1 = 0;
//        swd_read_core_register(1, &r1);
//
//        uint32_t r2 = 0;
//        swd_read_core_register(2, &r2);
//
//        uint32_t r15 = 0;
//        swd_read_core_register(15, &r15);
//        ESP_LOGW(DAP_TAG, "R1 = 0x%x, R2 = 0x%x, R15 (PC) = 0x%x", r1, r2, r15);

        if (state.r[0] != 0) {
            uint32_t r1 = 0;
            swd_read_core_register(1, &r1);

            uint32_t r2 = 0;
            swd_read_core_register(2, &r2);

            uint32_t r15 = 0;
            swd_read_core_register(15, &r15);
            ESP_LOGW(DAP_TAG, "R1 = 0x%lx, R2 = 0x%lx, R15 (PC) = 0x%lx", r1, r2, r15);

            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

// SWD Reset
static esp_err_t IRAM_ATTR swd_reset(void)
{
    uint8_t tmp_in[8];
    uint8_t i = 0;

    for (i = 0; i < 8; i++) {
        tmp_in[i] = 0xff;
    }

    SWJ_Sequence(51, tmp_in);
    return ESP_OK;
}

// SWD Switch
static esp_err_t IRAM_ATTR swd_switch(uint16_t val)
{
    uint8_t tmp_in[2];
    tmp_in[0] = val & 0xff;
    tmp_in[1] = (val >> 8) & 0xff;
    SWJ_Sequence(16, tmp_in);
    return ESP_OK;
}


// SWD Read ID
esp_err_t IRAM_ATTR swd_read_idcode(uint32_t *id)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    uint8_t tmp_in[1];
    uint8_t tmp_out[4];
    tmp_in[0] = 0x00;
    SWJ_Sequence(8, tmp_in);

    if ((err = swd_read_dp(0, (uint32_t *)tmp_out)) != ESP_OK) {
        return err;
    }

    *id = (tmp_out[3] << 24) | (tmp_out[2] << 16) | (tmp_out[1] << 8) | tmp_out[0];
    return ESP_OK;
}


esp_err_t IRAM_ATTR JTAG2SWD()
{
    esp_err_t err;
    uint32_t tmp = 0;

    if ((err = swd_reset()) != ESP_OK) {
        return err;
    }

    if ((err = swd_switch(0xE79E)) != ESP_OK) {
        return err;
    }

    if ((err = swd_reset()) != ESP_OK) {
        return err;
    }

#ifdef CONFIG_ESP_SWD_USE_SPI
    swd_esp_spi_set_debug_capture(true);
#endif
    if ((err = swd_read_idcode(&tmp)) != ESP_OK) {
#ifdef CONFIG_ESP_SWD_USE_SPI
        swd_esp_spi_debug_t debug;
        swd_esp_spi_get_debug(&debug);
        swd_esp_spi_set_debug_capture(false);
        ESP_LOGE(DAP_TAG,
                 "SPI RX debug: response[31:0]=0x%08x, ACK=0x%x, "
                 "RX requested/programmed=%u/%u bits, FIFO=%08x:%08x, "
                 "post-done busy=%u",
                 (unsigned)debug.response_low, (unsigned)debug.ack,
                 (unsigned)debug.rx_requested_bits,
                 (unsigned)debug.rx_programmed_bits,
                 (unsigned)debug.rx_word1, (unsigned)debug.rx_word0,
                 (unsigned)debug.post_done_busy_count);
#endif
        ESP_LOGE(DAP_TAG, "Set transit fail");
        return err;
    }
#ifdef CONFIG_ESP_SWD_USE_SPI
    swd_esp_spi_set_debug_capture(false);
#endif

    return ESP_OK;
}



esp_err_t swd_init_debug(uint32_t ticks_to_wait)
{
    // Acquire before touching either shared DAP state or target pins.
    esp_err_t err = swd_begin_session(ticks_to_wait);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t tmp = 0;
    int i = 0;
    int timeout = 100;
    // init dap state with fake values
    dap_state.select = 0xffffffff;
    dap_state.csw = 0xffffffff;

#if CONFIG_ESP_SWD_BOOT_PIN != -1
    gpio_config_t boot_pin_cfg = {};
    boot_pin_cfg.intr_type = GPIO_INTR_DISABLE;
    boot_pin_cfg.mode = GPIO_MODE_OUTPUT;
    boot_pin_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    boot_pin_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    boot_pin_cfg.pin_bit_mask = (1 << CONFIG_ESP_SWD_BOOT_PIN);
    if ((err = gpio_config(&boot_pin_cfg)) != ESP_OK) {
        swd_off();
        return err;
    }

#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    ESP_LOGI(DAP_TAG, "Keeping BOOT0 deasserted for SWD");
    err = gpio_set_level(CONFIG_ESP_SWD_BOOT_PIN, 0);
#else
    ESP_LOGI(DAP_TAG, "Asserting BOOT0 pin");
    err = gpio_set_level(CONFIG_ESP_SWD_BOOT_PIN, 1);
#endif
    if (err != ESP_OK) {
        swd_off();
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
#endif

    int8_t retries = 4;
    int8_t do_abort = 0;
    do {
        if (do_abort) {
            //do an abort on stale target, then reset the device
            // Best-effort recovery; retain the next connection attempt's
            // error rather than replacing it with an abort/cleanup result.
            (void)swd_write_dp(DP_ABORT, DAPABORT);
            PIN_nRESET_OUT(0);
            vTaskDelay(pdMS_TO_TICKS(20));
            PIN_nRESET_OUT(1);
            vTaskDelay(pdMS_TO_TICKS(20));
            do_abort = 0;
        }
        err = swd_init(0);
        if (err != ESP_OK) {
            return err;
        }

        if ((err = JTAG2SWD()) != ESP_OK) {
            ESP_LOGE(DAP_TAG, "JTAG2SWD fail");
            do_abort = 1;
            continue;
        }

        if ((err = swd_clear_errors()) != ESP_OK) {
            ESP_LOGE(DAP_TAG, "Clear error fail");
            do_abort = 1;
            continue;
        }

        if ((err = swd_write_dp(DP_SELECT, 0)) != ESP_OK) {
            ESP_LOGE(DAP_TAG, "SELECT DP fail");
            do_abort = 1;
            continue;

        }

        // Power up
        if ((err = swd_write_dp(DP_CTRL_STAT, CSYSPWRUPREQ | CDBGPWRUPREQ)) != ESP_OK) {
            ESP_LOGE(DAP_TAG, "Power up fail");
            do_abort = 1;
            continue;
        }

        for (i = 0; i < timeout; i++) {
            if ((err = swd_read_dp(DP_CTRL_STAT, &tmp)) != ESP_OK) {
                ESP_LOGE(DAP_TAG, "DP_CTRL_STAT fail");
                do_abort = 1;
                break;
            }
            if ((tmp & (CDBGPWRUPACK | CSYSPWRUPACK)) == (CDBGPWRUPACK | CSYSPWRUPACK)) {
                // Break from loop if powerup is complete
                break;
            }
        }
        if ((i == timeout) || (do_abort == 1)) {
            // Unable to powerup DP
            if (i == timeout) err = ESP_ERR_TIMEOUT;
            ESP_LOGE(DAP_TAG, "Unable to powerup DP");
            do_abort = 1;
            continue;
        }

        if ((err = swd_write_dp(DP_CTRL_STAT, CSYSPWRUPREQ | CDBGPWRUPREQ | TRNNORMAL | MASKLANE)) != ESP_OK) {
            ESP_LOGE(DAP_TAG, "Set transit fail");
            do_abort = 1;
            continue;
        }

        if ((err = swd_write_dp(DP_SELECT, 0)) != ESP_OK) {
            ESP_LOGE(DAP_TAG, "Unselect DP fail");
            do_abort = 1;
            continue;
        }

        return ESP_OK;

    } while (--retries > 0);

    swd_off();
    return err;
}

esp_err_t IRAM_ATTR swd_halt_target()
{
    esp_err_t err;
    if ((err = swd_write_word(DBG_HCSR, DBGKEY | C_DEBUGEN | C_HALT)) != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

void swd_trigger_nrst()
{
    gpio_set_level(CONFIG_ESP_SWD_BOOT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    PIN_nRESET_OUT(0);
    vTaskDelay(pdMS_TO_TICKS(350));
    PIN_nRESET_OUT(1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t swd_flash_syscall_exec_async(const program_syscall_t *sys_call, uint32_t entry, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4)
{
    if (sys_call == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    DEBUG_STATE state = {{0}, 0};
    // Call flash algorithm function on target and wait for result.
    state.r[0]     = arg1;                   // R0: Argument 1
    state.r[1]     = arg2;                   // R1: Argument 2
    state.r[2]     = arg3;                   // R2: Argument 3
    state.r[3]     = arg4;                   // R3: Argument 4
    state.r[9]     = sys_call->static_base;    // SB: Static Base
    state.r[13]    = sys_call->stack_pointer;  // SP: Stack Pointer
    state.r[14]    = sys_call->breakpoint;     // LR: Exit Point
    state.r[15]    = entry;                        // PC: Entry Point
    state.xpsr     = 0x01000000;          // xPSR: T = 1, ISR = 0

    if ((err = swd_write_debug_state(&state)) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to set state");
        return err;
    }

    return ESP_OK;
}

esp_err_t swd_flash_syscall_wait_result(flash_algo_return_t return_type, uint32_t *ret_out)
{
    if (return_type != FLASHALGO_RETURN_BOOL &&
        return_type != FLASHALGO_RETURN_POINTER && return_type != FLASHALGO_RETURN_VALUE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    DEBUG_STATE state = {{0}, 0};
    if ((err = swd_wait_until_halted()) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to halt");
        return err;
    }

    if ((err = swd_read_core_register(0, &state.r[0])) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to read register");
        return err;
    }

    //remove the C_MASKINTS
    if ((err = swd_write_word(DBG_HCSR, DBGKEY | C_DEBUGEN | C_HALT)) != ESP_OK) {
        ESP_LOGE(DAP_TAG, "Failed to halt again");
        return err;
    }

    if (return_type == FLASHALGO_RETURN_POINTER) {
        // Flash verify functions return pointer to byte following the buffer if successful.
        ESP_LOGE(DAP_TAG, "Async exec doesn't support POINTER return type");
        return ESP_ERR_NOT_SUPPORTED;
    } else if (return_type == FLASHALGO_RETURN_VALUE) {
        if (ret_out != NULL) {
            *ret_out = state.r[0];
        }
    } else {
        if (state.r[0] != 0) {
            uint32_t r1 = 0;
            swd_read_core_register(1, &r1);

            uint32_t r2 = 0;
            swd_read_core_register(2, &r2);

            uint32_t r15 = 0;
            swd_read_core_register(15, &r15);
            ESP_LOGW(DAP_TAG, "R1 = 0x%lx, R2 = 0x%lx, R15 (PC) = 0x%lx", r1, r2, r15);

            return ESP_FAIL;
        }
    }

    return ESP_OK;
}
