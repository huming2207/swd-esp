#include <sdkconfig.h>

#ifdef CONFIG_ESP_SWD_USE_PARLIO

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <driver/parlio_rx.h>
#include <driver/parlio_tx.h>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>

#include "DAP_config.h"
#include "DAP.h"
#include "SW_DP_PARLIO.h"

#define SWD_PARLIO_DATA_OUT    (1U << 0)
#define SWD_PARLIO_DATA_NOE    (1U << 1)
#define SWD_PARLIO_DATA_DIR1   (1U << 2)
#define SWD_PARLIO_DATA_DIR2   (1U << 3)
#define SWD_PARLIO_CLOCK_GATE  (1U << 15)

#define SWD_PARLIO_TX_WORDS    512U
#define SWD_PARLIO_RX_BYTES    128U
#define SWD_PARLIO_TIMEOUT_MS  100U

static const char *TAG = "swd_parlio";
static parlio_tx_unit_handle_t swd_tx_unit;
static parlio_rx_unit_handle_t swd_rx_unit;
static parlio_rx_delimiter_handle_t swd_rx_delimiters[SWD_PARLIO_RX_BYTES + 1U];
static DMA_ATTR uint16_t swd_tx_buffer[SWD_PARLIO_TX_WORDS];
static DMA_ATTR uint8_t swd_rx_buffer[SWD_PARLIO_RX_BYTES];
static bool swd_parlio_initialized;
static bool swd_parlio_host_owns_bus = true;
static uint32_t swd_parlio_data_idle = 1U;

static const DRAM_ATTR uint8_t swd_parlio_request_packet[16] = {
    0x81, 0xA3, 0xA5, 0x87, 0xA9, 0x8B, 0x8D, 0xAF,
    0xB1, 0x93, 0x95, 0xB7, 0x99, 0xBB, 0xBD, 0x9F,
};

static inline uint16_t swd_parlio_word(uint32_t data, uint32_t noe,
                                       uint32_t host_direction,
                                       uint32_t clock_gate)
{
    return (data ? SWD_PARLIO_DATA_OUT : 0U) |
           (noe ? SWD_PARLIO_DATA_NOE : 0U) |
           (host_direction ? SWD_PARLIO_DATA_DIR1 : 0U) |
           (clock_gate ? SWD_PARLIO_CLOCK_GATE : 0U);
}

static inline uint16_t swd_parlio_host_word(uint32_t data, bool isolated,
                                            bool clocked)
{
    return swd_parlio_word(data, isolated, 1U, clocked);
}

static inline uint16_t swd_parlio_target_word(uint32_t data, bool isolated,
                                              bool clocked)
{
    return swd_parlio_word(data, isolated, 0U, clocked);
}

static void swd_parlio_cpu_guard(void)
{
#if (CONFIG_ESP_SWD_TURNAROUND_DELAY_US > 0) || (CONFIG_ESP_SWD_TURNAROUND_DELAY_NS > 0)
    const uint32_t start = esp_cpu_get_cycle_count();
    while ((esp_cpu_get_cycle_count() - start) < ESP_SWD_TURNAROUND_GUARD_CYCLES) {
    }
#endif
}

static uint32_t swd_parlio_guard_words(void)
{
    const uint64_t guard_ns = ESP_SWD_TURNAROUND_GUARD_NS;
    const uint64_t words =
        ((uint64_t)CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ * guard_ns + 999999999ULL) /
        1000000000ULL;
    return words == 0U ? 1U : (uint32_t)words;
}

static esp_err_t swd_parlio_tx(size_t word_count, uint16_t idle_word)
{
    const parlio_transmit_config_t config = {
        .idle_value = idle_word,
    };
    esp_err_t ret = parlio_tx_unit_transmit(
        swd_tx_unit, swd_tx_buffer, word_count * 16U, &config);
    if (ret == ESP_OK) {
        ret = parlio_tx_unit_wait_all_done(swd_tx_unit, SWD_PARLIO_TIMEOUT_MS);
    }
    return ret;
}

static esp_err_t swd_parlio_isolate_host(uint32_t next_data)
{
    swd_tx_buffer[0] = swd_parlio_host_word(next_data, true, false);
    esp_err_t ret = swd_parlio_tx(
        1U, swd_parlio_host_word(next_data, true, false));
    if (ret != ESP_OK) {
        return ret;
    }
    gpio_ll_output_disable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
    gpio_ll_input_enable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
    swd_parlio_cpu_guard();
    swd_parlio_host_owns_bus = false;
    swd_parlio_data_idle = next_data & 1U;
    return ESP_OK;
}

static esp_err_t swd_parlio_host_bits(uint64_t bits, uint32_t count,
                                      bool isolate_after)
{
    if (count > 64U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t initial_data = (uint32_t)bits & 1U;
    size_t words = 0U;

    if (!swd_parlio_host_owns_bus) {
        swd_parlio_cpu_guard();
        gpio_ll_input_disable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
        gpio_ll_output_enable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
        swd_tx_buffer[words++] =
            swd_parlio_host_word(initial_data, true, false);
        const uint32_t guard_words = swd_parlio_guard_words();
        for (uint32_t i = 0U; i < guard_words; i++) {
            swd_tx_buffer[words++] =
                swd_parlio_host_word(initial_data, false, false);
        }
        swd_parlio_host_owns_bus = true;
    }

    for (uint32_t i = 0U; i < count; i++) {
        const uint32_t bit = (uint32_t)(bits >> i) & 1U;
        swd_tx_buffer[words++] = swd_parlio_host_word(bit, false, true);
        swd_parlio_data_idle = bit;
    }

    const uint16_t idle_word = swd_parlio_host_word(
        swd_parlio_data_idle, isolate_after, false);
    if (words == 0U) {
        swd_tx_buffer[words++] = idle_word;
    }
    esp_err_t ret = swd_parlio_tx(words, idle_word);
    if ((ret == ESP_OK) && isolate_after) {
        gpio_ll_output_disable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
        gpio_ll_input_enable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
        swd_parlio_cpu_guard();
        swd_parlio_host_owns_bus = false;
    }
    return ret;
}

static esp_err_t swd_parlio_get_delimiter(
    uint32_t count, parlio_rx_delimiter_handle_t *delimiter)
{
    if ((count == 0U) || (count > SWD_PARLIO_RX_BYTES)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (swd_rx_delimiters[count] == NULL) {
        const parlio_rx_soft_delimiter_config_t config = {
            .sample_edge = PARLIO_SAMPLE_EDGE_POS,
            .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
            .eof_data_len = count,
            .timeout_ticks = 0U,
        };
        esp_err_t ret = parlio_new_rx_soft_delimiter(
            &config, &swd_rx_delimiters[count]);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    *delimiter = swd_rx_delimiters[count];
    return ESP_OK;
}

static esp_err_t swd_parlio_target_bits(uint32_t count, uint32_t next_data,
                                        uint64_t *bits)
{
    parlio_rx_delimiter_handle_t delimiter;
    esp_err_t ret = swd_parlio_get_delimiter(count, &delimiter);
    if (ret != ESP_OK) {
        return ret;
    }

    if (swd_parlio_host_owns_bus) {
        ret = swd_parlio_isolate_host(next_data);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    memset(swd_rx_buffer, 0, count);
    ret = parlio_rx_soft_delimiter_start_stop(swd_rx_unit, delimiter, true);
    if (ret != ESP_OK) {
        return ret;
    }
    const parlio_receive_config_t receive_config = {
        .delimiter = delimiter,
    };
    ret = parlio_rx_unit_receive(
        swd_rx_unit, swd_rx_buffer, count, &receive_config);
    if (ret != ESP_OK) {
        parlio_rx_soft_delimiter_start_stop(swd_rx_unit, delimiter, false);
        return ret;
    }

    size_t words = 0U;
    swd_tx_buffer[words++] = swd_parlio_target_word(next_data, true, false);
    const uint32_t guard_words = swd_parlio_guard_words();
    for (uint32_t i = 0U; i < guard_words; i++) {
        swd_tx_buffer[words++] =
            swd_parlio_target_word(next_data, false, false);
    }
    for (uint32_t i = 0U; i < count; i++) {
        swd_tx_buffer[words++] =
            swd_parlio_target_word(next_data, false, true);
    }

    ret = swd_parlio_tx(
        words, swd_parlio_target_word(next_data, true, false));
    if (ret == ESP_OK) {
        ret = parlio_rx_unit_wait_all_done(swd_rx_unit, SWD_PARLIO_TIMEOUT_MS);
    }
    parlio_rx_soft_delimiter_start_stop(swd_rx_unit, delimiter, false);
    if (ret != ESP_OK) {
        parlio_rx_unit_disable(swd_rx_unit);
        parlio_rx_unit_enable(swd_rx_unit, true);
        return ret;
    }

    if (bits != NULL) {
        uint64_t value = 0U;
        const uint32_t packed_count = count > 64U ? 64U : count;
        for (uint32_t i = 0U; i < packed_count; i++) {
            value |= (uint64_t)(swd_rx_buffer[i] & 1U) << i;
        }
        *bits = value;
    }
    swd_parlio_data_idle = next_data & 1U;
    return ESP_OK;
}

static uint64_t swd_parlio_load_bits(uint32_t bit_offset, uint32_t count,
                                     const uint8_t *data)
{
    uint64_t bits = 0U;
    for (uint32_t i = 0U; i < count; i++) {
        const uint32_t source_bit = bit_offset + i;
        bits |= (uint64_t)((data[source_bit >> 3U] >>
                            (source_bit & 7U)) & 1U) << i;
    }
    return bits;
}

esp_err_t swd_esp_parlio_init(void)
{
    if (swd_parlio_initialized) {
        return ESP_OK;
    }

    parlio_tx_unit_config_t tx_config = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .clk_in_gpio_num = -1,
        .output_clk_freq_hz = CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ,
        .data_width = 16,
        .data_gpio_nums = {
            [0] = CONFIG_ESP_SWD_DATA_OUT_PIN,
            [1] = -1,
            [2] = CONFIG_ESP_SWD_DATA_DIR1_PIN,
            [3] = CONFIG_ESP_SWD_DATA_DIR2_PIN,
            [4 ... 15] = -1,
        },
        .clk_out_gpio_num = CONFIG_ESP_SWD_CLK_PIN,
        .valid_gpio_num = -1,
        .trans_queue_depth = 1,
        .max_transfer_size = sizeof(swd_tx_buffer),
        .dma_burst_size = 16,
        .shift_edge = PARLIO_SHIFT_EDGE_NEG,
        .flags.clk_gate_en = 1,
    };
    esp_err_t ret = parlio_new_tx_unit(&tx_config, &swd_tx_unit);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = parlio_tx_unit_enable(swd_tx_unit);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Prime a safe idle value before PARLIO is connected to shared /OE. */
    swd_tx_buffer[0] = swd_parlio_host_word(1U, true, false);
    ret = swd_parlio_tx(1U, swd_parlio_host_word(1U, true, false));
    if (ret != ESP_OK) {
        return ret;
    }
    esp_rom_gpio_connect_out_signal(CONFIG_ESP_SWD_DATA_NOE_PIN,
                                    PARLIO_TX_DATA1_PAD_OUT_IDX, false, false);

    parlio_rx_unit_config_t rx_config = {
        .trans_queue_depth = 1,
        .max_recv_size = sizeof(swd_rx_buffer),
        .dma_burst_size = 16,
        .data_width = 8,
        .clk_src = PARLIO_CLK_SRC_EXTERNAL,
        .ext_clk_freq_hz = CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ,
        .exp_clk_freq_hz = CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ,
        .clk_in_gpio_num = CONFIG_ESP_SWD_CLK_PIN,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .data_gpio_nums = {
            [0] = CONFIG_ESP_SWD_DATA_IN_PIN,
            [1 ... 7] = -1,
        },
        .flags.free_clk = 0,
    };
    ret = parlio_new_rx_unit(&rx_config, &swd_rx_unit);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = parlio_rx_unit_enable(swd_rx_unit, true);
    if (ret != ESP_OK) {
        return ret;
    }

    swd_parlio_initialized = true;
    return ESP_OK;
}

void swd_esp_parlio_setup(void)
{
    if (!swd_parlio_initialized) {
        return;
    }
    esp_err_t ret = swd_parlio_host_bits(1U, 0U, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup failed: %s", esp_err_to_name(ret));
    }
}

void swd_esp_parlio_off(void)
{
    if (!swd_parlio_initialized) {
        return;
    }
    esp_err_t ret;
    if (swd_parlio_host_owns_bus) {
        ret = swd_parlio_isolate_host(1U);
    } else {
        swd_tx_buffer[0] = swd_parlio_target_word(1U, true, false);
        ret = swd_parlio_tx(
            1U, swd_parlio_target_word(1U, true, false));
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "shutdown isolation failed: %s", esp_err_to_name(ret));
    }
}

uint32_t swd_esp_parlio_clock_level(void)
{
    return 0U;
}

void swd_esp_parlio_set_clock_idle(uint32_t level)
{
    (void)level;
}

uint32_t swd_esp_parlio_data_level(void)
{
    return swd_parlio_data_idle;
}

void swd_esp_parlio_set_data_idle(uint32_t level)
{
    level &= 1U;
    if (!swd_parlio_initialized || !swd_parlio_host_owns_bus) {
        swd_parlio_data_idle = level;
        return;
    }
    const esp_err_t ret = swd_parlio_host_bits(level, 0U, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to set SWDIO idle: %s", esp_err_to_name(ret));
    }
}

uint32_t swd_esp_parlio_data_in_level(void)
{
    return gpio_ll_get_level(&GPIO, CONFIG_ESP_SWD_DATA_IN_PIN);
}

void swd_esp_parlio_swj_sequence(uint32_t count, const uint8_t *data)
{
    uint32_t offset = 0U;
    while (count != 0U) {
        const uint32_t chunk = count > 64U ? 64U : count;
        const esp_err_t ret = swd_parlio_host_bits(
            swd_parlio_load_bits(offset, chunk, data), chunk, false);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SWJ sequence failed: %s", esp_err_to_name(ret));
            return;
        }
        offset += chunk;
        count -= chunk;
    }
}

void swd_esp_parlio_swd_sequence(uint32_t info, const uint8_t *swdo,
                                 uint8_t *swdi)
{
    uint32_t count = info & SWD_SEQUENCE_CLK;
    if (count == 0U) {
        count = 64U;
    }
    if ((info & SWD_SEQUENCE_DIN) == 0U) {
        swd_esp_parlio_swj_sequence(count, swdo);
        return;
    }

    uint64_t bits = 0U;
    const esp_err_t ret = swd_parlio_target_bits(count, 1U, &bits);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SWD input sequence failed: %s", esp_err_to_name(ret));
        memset(swdi, 0, (count + 7U) / 8U);
        return;
    }
    for (uint32_t i = 0U; i < (count + 7U) / 8U; i++) {
        swdi[i] = (uint8_t)(bits >> (8U * i));
    }
    (void)swd_parlio_host_bits(1U, 0U, false);
}

uint8_t swd_esp_parlio_transfer(uint32_t request, uint32_t *data)
{
    const bool is_read = (request & DAP_TRANSFER_RnW) != 0U;
    const uint32_t turnaround = DAP_Data.swd_conf.turnaround;
    const uint32_t packet = swd_parlio_request_packet[request & 0x0FU];
    esp_err_t ret = swd_parlio_host_bits(packet, 8U, true);
    if (ret != ESP_OK) {
        return DAP_TRANSFER_ERROR;
    }

    uint64_t response = 0U;
    const uint32_t ack_cycles = turnaround + 3U + (is_read ? 0U : turnaround);
    ret = swd_parlio_target_bits(ack_cycles, 1U, &response);
    if (ret != ESP_OK) {
        return DAP_TRANSFER_ERROR;
    }
    uint32_t ack = (uint32_t)(response >> turnaround) & 0x07U;

    if (ack == DAP_TRANSFER_OK) {
        if (is_read) {
            uint64_t read_bits = 0U;
            ret = swd_parlio_target_bits(33U + turnaround, 1U, &read_bits);
            if (ret != ESP_OK) {
                return DAP_TRANSFER_ERROR;
            }
            const uint32_t value = (uint32_t)read_bits;
            const uint32_t parity = (uint32_t)(read_bits >> 32U) & 1U;
            if (((uint32_t)__builtin_parity(value) ^ parity) != 0U) {
                ack = DAP_TRANSFER_ERROR;
            }
            if (data != NULL) {
                *data = value;
            }
            ret = swd_parlio_host_bits(1U, 0U, false);
        } else {
            const uint32_t value = data != NULL ? *data : 0U;
            const uint64_t write_bits =
                (uint64_t)value | ((uint64_t)__builtin_parity(value) << 32U);
            ret = swd_parlio_host_bits(write_bits, 33U, false);
        }
        if (ret != ESP_OK) {
            return DAP_TRANSFER_ERROR;
        }

        if (request & DAP_TRANSFER_TIMESTAMP) {
            DAP_Data.timestamp = TIMESTAMP_GET();
        }
        uint32_t idle = DAP_Data.transfer.idle_cycles;
        while (idle != 0U) {
            const uint32_t chunk = idle > 64U ? 64U : idle;
            ret = swd_parlio_host_bits(0U, chunk, false);
            if (ret != ESP_OK) {
                return DAP_TRANSFER_ERROR;
            }
            idle -= chunk;
        }
        ret = swd_parlio_host_bits(1U, 0U, false);
        if (ret != ESP_OK) {
            return DAP_TRANSFER_ERROR;
        }
        return (uint8_t)ack;
    }

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        if (DAP_Data.swd_conf.data_phase && is_read) {
            ret = swd_parlio_target_bits(33U + turnaround, 1U, NULL);
        } else if (is_read) {
            ret = swd_parlio_target_bits(turnaround, 1U, NULL);
        }
        if (ret == ESP_OK) {
            if (DAP_Data.swd_conf.data_phase && !is_read) {
                ret = swd_parlio_host_bits(0U, 33U, false);
            } else {
                ret = swd_parlio_host_bits(1U, 0U, false);
            }
        }
        return ret == ESP_OK ? (uint8_t)ack : DAP_TRANSFER_ERROR;
    }

    ret = swd_parlio_target_bits(
        33U + (is_read ? turnaround : 0U), 1U, NULL);
    if (ret == ESP_OK) {
        ret = swd_parlio_host_bits(1U, 0U, false);
    }
    return ret == ESP_OK ? (uint8_t)ack : DAP_TRANSFER_ERROR;
}

#endif
