#include <sdkconfig.h>

#ifdef CONFIG_ESP_SWD_USE_PARLIO

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <esp_attr.h>
#include <esp_clk_tree.h>
#include <esp_cpu.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <esp_private/esp_clk_tree_common.h>
#include <esp_private/gdma.h>
#include <esp_private/periph_ctrl.h>
#include <hal/axi_dma_ll.h>
#include <hal/dma_types.h>
#include <hal/gpio_ll.h>
#include <hal/hal_utils.h>
#include <hal/parlio_ll.h>
#include <soc/clk_tree_defs.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>
#include <soc/parl_io_struct.h>

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
#define SWD_PARLIO_TIMEOUT_CYCLES \
    (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000U * SWD_PARLIO_TIMEOUT_MS)
#define SWD_PARLIO_DMA_EVENTS_TX AXI_DMA_LL_TX_EVENT_MASK
#define SWD_PARLIO_DMA_EVENTS_RX AXI_DMA_LL_RX_EVENT_MASK

static const char *TAG = "swd_parlio";
static gdma_channel_handle_t swd_tx_dma;
static gdma_channel_handle_t swd_rx_dma;
static axi_dma_dev_t *swd_dma_hw;
static uint32_t swd_tx_dma_channel;
static uint32_t swd_rx_dma_channel;
static DMA_ATTR __attribute__((aligned(16)))
    uint16_t swd_tx_buffer[2][SWD_PARLIO_TX_WORDS];
static DMA_ATTR __attribute__((aligned(16)))
    uint8_t swd_rx_buffer[SWD_PARLIO_RX_BYTES];
static DMA_ATTR dma_descriptor_align8_t swd_tx_desc[2];
static DMA_ATTR dma_descriptor_align8_t swd_rx_desc;
static bool swd_parlio_initialized;
static bool swd_parlio_host_owns_bus = true;
static uint32_t swd_parlio_data_idle = 1U;
static uint32_t swd_parlio_actual_clock;
static uint32_t swd_parlio_next_buffer;

typedef struct {
    uint32_t buffer_index;
    size_t word_count;
    uint16_t idle_word;
} swd_parlio_phase_t;

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

static inline bool swd_parlio_timed_out(uint32_t start)
{
    return (esp_cpu_get_cycle_count() - start) > SWD_PARLIO_TIMEOUT_CYCLES;
}

static swd_parlio_phase_t swd_parlio_new_phase(void)
{
    swd_parlio_phase_t phase = {
        .buffer_index = swd_parlio_next_buffer,
    };
    swd_parlio_next_buffer ^= 1U;
    return phase;
}

static void swd_parlio_abort(void)
{
    parlio_ll_rx_start_soft_recv(&PARL_IO, false);
    parlio_ll_tx_start(&PARL_IO, false);
    axi_dma_ll_tx_abort(swd_dma_hw, swd_tx_dma_channel, true);
    axi_dma_ll_rx_abort(swd_dma_hw, swd_rx_dma_channel, true);
    axi_dma_ll_tx_abort(swd_dma_hw, swd_tx_dma_channel, false);
    axi_dma_ll_rx_abort(swd_dma_hw, swd_rx_dma_channel, false);
    axi_dma_ll_tx_reset_channel(swd_dma_hw, swd_tx_dma_channel);
    axi_dma_ll_rx_reset_channel(swd_dma_hw, swd_rx_dma_channel);
}

static esp_err_t swd_parlio_tx_start(const swd_parlio_phase_t *phase)
{
    if ((phase->word_count == 0U) ||
        (phase->word_count > SWD_PARLIO_TX_WORDS)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t index = phase->buffer_index;
    const uint32_t byte_count = phase->word_count * sizeof(uint16_t);
    dma_descriptor_align8_t *desc = &swd_tx_desc[index];
    desc->dw0.size = byte_count;
    desc->dw0.length = byte_count;
    desc->dw0.err_eof = 0U;
    desc->dw0.suc_eof = 1U;
    desc->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    desc->buffer = swd_tx_buffer[index];
    desc->next = NULL;

    parlio_ll_clear_interrupt_status(&PARL_IO, PARLIO_LL_EVENT_TX_MASK);
    axi_dma_ll_tx_clear_interrupt_status(
        swd_dma_hw, swd_tx_dma_channel, SWD_PARLIO_DMA_EVENTS_TX);
    PERIPH_RCC_ATOMIC() {
        parlio_ll_tx_reset_clock(&PARL_IO);
        parlio_ll_tx_enable_clock(&PARL_IO, false);
    }
    parlio_ll_tx_reset_fifo(&PARL_IO);
    parlio_ll_tx_set_idle_data_value(&PARL_IO, phase->idle_word);
    parlio_ll_tx_set_eof_condition(&PARL_IO, PARLIO_LL_TX_EOF_COND_DMA_EOF);

    axi_dma_ll_tx_set_desc_addr(
        swd_dma_hw, swd_tx_dma_channel, (uint32_t)(uintptr_t)desc);
    axi_dma_ll_tx_start(swd_dma_hw, swd_tx_dma_channel);

    const uint32_t start = esp_cpu_get_cycle_count();
    while (!parlio_ll_tx_is_ready(&PARL_IO)) {
        if (swd_parlio_timed_out(start)) {
            swd_parlio_abort();
            return ESP_ERR_TIMEOUT;
        }
    }
    parlio_ll_tx_start(&PARL_IO, true);
    PERIPH_RCC_ATOMIC() {
        parlio_ll_tx_enable_clock(&PARL_IO, true);
    }
    return ESP_OK;
}

static esp_err_t swd_parlio_tx_wait(void)
{
    const uint32_t start = esp_cpu_get_cycle_count();
    while ((PARL_IO.int_raw.val & PARLIO_LL_EVENT_TX_EOF) == 0U) {
        if (swd_parlio_timed_out(start)) {
            swd_parlio_abort();
            return ESP_ERR_TIMEOUT;
        }
    }
    parlio_ll_clear_interrupt_status(&PARL_IO, PARLIO_LL_EVENT_TX_EOF);
    parlio_ll_tx_start(&PARL_IO, false);
    return ESP_OK;
}

static esp_err_t swd_parlio_tx(const swd_parlio_phase_t *phase)
{
    esp_err_t ret = swd_parlio_tx_start(phase);
    return ret == ESP_OK ? swd_parlio_tx_wait() : ret;
}

static esp_err_t swd_parlio_prepare_host_phase(
    swd_parlio_phase_t *phase, uint64_t bits, uint32_t count,
    bool acquire_host, bool isolate_after)
{
    if (count > 64U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t *buffer = swd_tx_buffer[phase->buffer_index];
    const uint32_t initial_data = (uint32_t)bits & 1U;
    uint32_t final_data = acquire_host ? initial_data : swd_parlio_data_idle;
    if (count == 0U) {
        final_data = initial_data;
    }
    size_t words = 0U;

    if (acquire_host) {
        buffer[words++] = swd_parlio_host_word(initial_data, true, false);
        const uint32_t guard_words = swd_parlio_guard_words();
        for (uint32_t i = 0U; i < guard_words; i++) {
            buffer[words++] =
                swd_parlio_host_word(initial_data, false, false);
        }
    }
    for (uint32_t i = 0U; i < count; i++) {
        final_data = (uint32_t)(bits >> i) & 1U;
        buffer[words++] = swd_parlio_host_word(final_data, false, true);
    }
    if ((words == 0U) && isolate_after) {
        buffer[words++] = swd_parlio_host_word(final_data, true, false);
    }

    phase->word_count = words;
    phase->idle_word = swd_parlio_host_word(
        final_data, isolate_after, false);
    return ESP_OK;
}

static esp_err_t swd_parlio_run_host_phase(
    const swd_parlio_phase_t *phase, uint32_t final_data,
    bool acquire_host, bool isolate_after)
{
    if (acquire_host) {
        swd_parlio_cpu_guard();
        gpio_ll_input_disable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
        gpio_ll_output_enable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
    }

    esp_err_t ret = phase->word_count == 0U ? ESP_OK : swd_parlio_tx(phase);
    if (ret != ESP_OK) {
        return ret;
    }
    swd_parlio_data_idle = final_data & 1U;
    swd_parlio_host_owns_bus = !isolate_after;
    if (isolate_after) {
        gpio_ll_output_disable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
        gpio_ll_input_enable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
        swd_parlio_cpu_guard();
    }
    return ESP_OK;
}

static esp_err_t swd_parlio_isolate_host(uint32_t next_data)
{
    swd_parlio_phase_t phase = swd_parlio_new_phase();
    esp_err_t ret = swd_parlio_prepare_host_phase(
        &phase, next_data, 0U, false, true);
    if (ret == ESP_OK) {
        ret = swd_parlio_run_host_phase(
            &phase, next_data, false, true);
    }
    return ret;
}

static esp_err_t swd_parlio_host_bits(uint64_t bits, uint32_t count,
                                      bool isolate_after)
{
    const uint32_t initial_data = (uint32_t)bits & 1U;
    if ((count == 0U) && swd_parlio_host_owns_bus && !isolate_after) {
        swd_parlio_data_idle = initial_data;
        parlio_ll_tx_set_idle_data_value(
            &PARL_IO, swd_parlio_host_word(initial_data, false, false));
        return ESP_OK;
    }

    const bool acquire_host = !swd_parlio_host_owns_bus;
    const uint32_t final_data = count == 0U
                                    ? initial_data
                                    : (uint32_t)(bits >> (count - 1U)) & 1U;
    swd_parlio_phase_t phase = swd_parlio_new_phase();
    esp_err_t ret = swd_parlio_prepare_host_phase(
        &phase, bits, count, acquire_host, isolate_after);
    if (ret == ESP_OK) {
        ret = swd_parlio_run_host_phase(
            &phase, final_data, acquire_host, isolate_after);
    }
    return ret;
}

static esp_err_t swd_parlio_prepare_target_phase(
    swd_parlio_phase_t *phase, uint32_t count, uint32_t next_data)
{
    if ((count == 0U) || (count > SWD_PARLIO_RX_BYTES)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t *buffer = swd_tx_buffer[phase->buffer_index];
    size_t words = 0U;
    buffer[words++] = swd_parlio_target_word(next_data, true, false);
    const uint32_t guard_words = swd_parlio_guard_words();
    for (uint32_t i = 0U; i < guard_words; i++) {
        buffer[words++] = swd_parlio_target_word(next_data, false, false);
    }
    for (uint32_t i = 0U; i < count; i++) {
        buffer[words++] = swd_parlio_target_word(next_data, false, true);
    }
    phase->word_count = words;
    phase->idle_word = swd_parlio_target_word(next_data, true, false);
    return ESP_OK;
}

static esp_err_t swd_parlio_target_start(
    const swd_parlio_phase_t *phase, uint32_t count)
{
    dma_descriptor_align8_t *desc = &swd_rx_desc;
    desc->dw0.size = count;
    desc->dw0.length = 0U;
    desc->dw0.err_eof = 0U;
    desc->dw0.suc_eof = 0U;
    desc->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    desc->buffer = swd_rx_buffer;
    desc->next = NULL;

    parlio_ll_rx_start_soft_recv(&PARL_IO, false);
    parlio_ll_rx_set_recv_bit_len(&PARL_IO, count * 8U);
    axi_dma_ll_rx_clear_interrupt_status(
        swd_dma_hw, swd_rx_dma_channel, SWD_PARLIO_DMA_EVENTS_RX);
    axi_dma_ll_rx_set_desc_addr(
        swd_dma_hw, swd_rx_dma_channel, (uint32_t)(uintptr_t)desc);
    axi_dma_ll_rx_start(swd_dma_hw, swd_rx_dma_channel);
    parlio_ll_rx_start_soft_recv(&PARL_IO, true);

    const esp_err_t ret = swd_parlio_tx_start(phase);
    if (ret != ESP_OK) {
        parlio_ll_rx_start_soft_recv(&PARL_IO, false);
    }
    return ret;
}

static esp_err_t swd_parlio_target_finish(uint32_t count, uint32_t next_data,
                                          uint64_t *bits)
{
    const uint32_t start = esp_cpu_get_cycle_count();
    uint32_t tx_status;
    uint32_t rx_status;
    do {
        tx_status = PARL_IO.int_raw.val;
        rx_status = axi_dma_ll_rx_get_interrupt_status(
            swd_dma_hw, swd_rx_dma_channel, true);
        if (swd_parlio_timed_out(start)) {
            swd_parlio_abort();
            return ESP_ERR_TIMEOUT;
        }
    } while (((tx_status & PARLIO_LL_EVENT_TX_EOF) == 0U) ||
             ((rx_status & GDMA_LL_EVENT_RX_SUC_EOF) == 0U));

    parlio_ll_clear_interrupt_status(&PARL_IO, PARLIO_LL_EVENT_TX_EOF);
    parlio_ll_tx_start(&PARL_IO, false);
    parlio_ll_rx_start_soft_recv(&PARL_IO, false);
    axi_dma_ll_rx_clear_interrupt_status(
        swd_dma_hw, swd_rx_dma_channel, SWD_PARLIO_DMA_EVENTS_RX);
    if ((rx_status & (GDMA_LL_EVENT_RX_ERR_EOF |
                      GDMA_LL_EVENT_RX_DESC_ERROR |
                      GDMA_LL_EVENT_RX_DESC_EMPTY)) != 0U ||
        swd_rx_desc.dw0.err_eof != 0U || swd_rx_desc.dw0.length < count) {
        swd_parlio_abort();
        return ESP_FAIL;
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

static esp_err_t swd_parlio_target_bits(uint32_t count, uint32_t next_data,
                                        uint64_t *bits)
{
    if (swd_parlio_host_owns_bus) {
        const esp_err_t isolate_ret = swd_parlio_isolate_host(next_data);
        if (isolate_ret != ESP_OK) {
            return isolate_ret;
        }
    }

    swd_parlio_phase_t phase = swd_parlio_new_phase();
    esp_err_t ret = swd_parlio_prepare_target_phase(
        &phase, count, next_data);
    if (ret == ESP_OK) {
        ret = swd_parlio_target_start(&phase, count);
    }
    if (ret == ESP_OK) {
        ret = swd_parlio_target_finish(count, next_data, bits);
    }
    return ret;
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

    gdma_channel_alloc_config_t dma_config = {};
    esp_err_t ret = gdma_new_axi_channel(
        &dma_config, &swd_tx_dma, &swd_rx_dma);
    if (ret != ESP_OK) {
        return ret;
    }
    const gdma_trigger_t trigger =
        GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_PARLIO, 0);
    ret = gdma_connect(swd_tx_dma, trigger);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = gdma_connect(swd_rx_dma, trigger);
    if (ret != ESP_OK) {
        return ret;
    }
    const gdma_strategy_config_t tx_strategy = {
        .owner_check = false,
        .auto_update_desc = false,
        .eof_till_data_popped = true,
    };
    const gdma_strategy_config_t rx_strategy = {
        .owner_check = false,
        .auto_update_desc = true,
    };
    ret = gdma_apply_strategy(swd_tx_dma, &tx_strategy);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = gdma_apply_strategy(swd_rx_dma, &rx_strategy);
    if (ret != ESP_OK) {
        return ret;
    }
    const gdma_transfer_config_t transfer_config = {
        .max_data_burst_size = 16U,
        .access_ext_mem = false,
    };
    ret = gdma_config_transfer(swd_tx_dma, &transfer_config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = gdma_config_transfer(swd_rx_dma, &transfer_config);
    if (ret != ESP_OK) {
        return ret;
    }

    int tx_group;
    int tx_channel;
    int rx_group;
    int rx_channel;
    ret = gdma_get_group_channel_id(swd_tx_dma, &tx_group, &tx_channel);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = gdma_get_group_channel_id(swd_rx_dma, &rx_group, &rx_channel);
    if (ret != ESP_OK) {
        return ret;
    }
    if ((tx_group != GDMA_LL_AXI_GROUP_START_ID) ||
        (rx_group != GDMA_LL_AXI_GROUP_START_ID)) {
        return ESP_ERR_INVALID_STATE;
    }
    swd_dma_hw = AXI_DMA_LL_GET_HW(0);
    swd_tx_dma_channel = (uint32_t)tx_channel;
    swd_rx_dma_channel = (uint32_t)rx_channel;
    axi_dma_ll_tx_enable_interrupt(
        swd_dma_hw, swd_tx_dma_channel, SWD_PARLIO_DMA_EVENTS_TX, false);
    axi_dma_ll_rx_enable_interrupt(
        swd_dma_hw, swd_rx_dma_channel, SWD_PARLIO_DMA_EVENTS_RX, false);
    axi_dma_ll_tx_clear_interrupt_status(
        swd_dma_hw, swd_tx_dma_channel, SWD_PARLIO_DMA_EVENTS_TX);
    axi_dma_ll_rx_clear_interrupt_status(
        swd_dma_hw, swd_rx_dma_channel, SWD_PARLIO_DMA_EVENTS_RX);

    ret = esp_clk_tree_enable_src(
        (soc_module_clk_t)PARLIO_CLK_SRC_DEFAULT, true);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t source_clock_hz = 0U;
    ret = esp_clk_tree_src_get_freq_hz(
        (soc_module_clk_t)PARLIO_CLK_SRC_DEFAULT,
        ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &source_clock_hz);
    if ((ret != ESP_OK) || (source_clock_hz == 0U)) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    hal_utils_clk_div_t tx_div = {};
    hal_utils_clk_info_t tx_clk_info = {
        .src_freq_hz = source_clock_hz,
        .exp_freq_hz = CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ,
        .max_integ = PARLIO_LL_TX_MAX_CLK_INT_DIV,
        .min_integ = 1,
        .round_opt = HAL_DIV_ROUND,
    };
#if PARLIO_LL_TX_MAX_CLK_FRACT_DIV
    tx_clk_info.max_fract = PARLIO_LL_TX_MAX_CLK_FRACT_DIV;
    swd_parlio_actual_clock =
        hal_utils_calc_clk_div_frac_accurate(&tx_clk_info, &tx_div);
#else
    swd_parlio_actual_clock =
        hal_utils_calc_clk_div_integer(&tx_clk_info, &tx_div.integer);
#endif
    if (swd_parlio_actual_clock == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const hal_utils_clk_div_t rx_div = {
        .integer = 1U,
    };

    PERIPH_RCC_ATOMIC() {
        parlio_ll_enable_bus_clock(0, true);
        parlio_ll_reset_register(0);
        parlio_ll_tx_enable_clock(&PARL_IO, true);
        parlio_ll_tx_set_clock_source(&PARL_IO, PARLIO_CLK_SRC_DEFAULT);
        parlio_ll_tx_set_clock_div(&PARL_IO, &tx_div);
        parlio_ll_rx_set_clock_source(&PARL_IO, PARLIO_CLK_SRC_EXTERNAL);
        parlio_ll_rx_set_clock_div(&PARL_IO, &rx_div);
    }

    PERIPH_RCC_ATOMIC() {
        parlio_ll_tx_reset_clock(&PARL_IO);
        parlio_ll_rx_reset_clock(&PARL_IO);
    }
    parlio_ll_tx_reset_fifo(&PARL_IO);
    parlio_ll_rx_reset_fifo(&PARL_IO);
    parlio_ll_tx_set_bus_width(&PARL_IO, 16U);
    parlio_ll_tx_enable_clock_gating(&PARL_IO, true);
    parlio_ll_tx_clock_gating_from_valid(&PARL_IO, false);
    parlio_ll_tx_set_valid_delay(&PARL_IO, 0U, 0U);
    parlio_ll_tx_set_shift_clock_edge(&PARL_IO, PARLIO_SHIFT_EDGE_NEG);
    parlio_ll_tx_set_eof_condition(&PARL_IO, PARLIO_LL_TX_EOF_COND_DMA_EOF);
    parlio_ll_tx_start(&PARL_IO, false);

    parlio_ll_rx_set_bus_width(&PARL_IO, 8U);
    parlio_ll_rx_set_soft_recv_mode(&PARL_IO);
    parlio_ll_rx_set_sample_clock_edge(&PARL_IO, PARLIO_SAMPLE_EDGE_POS);
    parlio_ll_rx_set_bit_pack_order(&PARL_IO, PARLIO_BIT_PACK_ORDER_LSB);
    parlio_ll_rx_set_eof_condition(&PARL_IO, PARLIO_LL_RX_EOF_COND_RX_FULL);
    parlio_ll_rx_enable_timeout(&PARL_IO, false);
    parlio_ll_rx_enable_clock_gating(&PARL_IO, false);
    parlio_ll_rx_start_soft_recv(&PARL_IO, false);
    parlio_ll_rx_start(&PARL_IO, true);
    PERIPH_RCC_ATOMIC() {
        parlio_ll_rx_enable_clock(&PARL_IO, true);
        parlio_ll_tx_enable_clock(&PARL_IO, false);
    }

    esp_rom_gpio_connect_out_signal(
        CONFIG_ESP_SWD_DATA_OUT_PIN, PARLIO_TX_DATA0_PAD_OUT_IDX,
        false, false);
    esp_rom_gpio_connect_out_signal(
        CONFIG_ESP_SWD_DATA_DIR1_PIN, PARLIO_TX_DATA2_PAD_OUT_IDX,
        false, false);
    esp_rom_gpio_connect_out_signal(
        CONFIG_ESP_SWD_DATA_DIR2_PIN, PARLIO_TX_DATA3_PAD_OUT_IDX,
        false, false);
    esp_rom_gpio_connect_out_signal(
        CONFIG_ESP_SWD_CLK_PIN, PARLIO_TX_CLK_PAD_OUT_IDX,
        false, false);
    gpio_ll_input_enable(&GPIO, CONFIG_ESP_SWD_CLK_PIN);
    esp_rom_gpio_connect_in_signal(
        CONFIG_ESP_SWD_CLK_PIN, PARLIO_RX_CLK_PAD_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(
        CONFIG_ESP_SWD_DATA_IN_PIN, PARLIO_RX_DATA0_PAD_IN_IDX, false);

    /* Prime a safe idle value before PARLIO is connected to shared /OE. */
    swd_parlio_phase_t prime = swd_parlio_new_phase();
    swd_tx_buffer[prime.buffer_index][0] =
        swd_parlio_host_word(1U, true, false);
    prime.word_count = 1U;
    prime.idle_word = swd_parlio_host_word(1U, true, false);
    ret = swd_parlio_tx(&prime);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_rom_gpio_connect_out_signal(
        CONFIG_ESP_SWD_DATA_NOE_PIN, PARLIO_TX_DATA1_PAD_OUT_IDX,
        false, false);

    swd_parlio_initialized = true;
    return ESP_OK;
}

uint32_t swd_esp_parlio_actual_clock_hz(void)
{
    return swd_parlio_actual_clock;
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
        swd_parlio_phase_t phase = swd_parlio_new_phase();
        swd_tx_buffer[phase.buffer_index][0] =
            swd_parlio_target_word(1U, true, false);
        phase.word_count = 1U;
        phase.idle_word = swd_parlio_target_word(1U, true, false);
        ret = swd_parlio_tx(&phase);
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
    swd_parlio_phase_t ack_phase = swd_parlio_new_phase();
    ret = swd_parlio_prepare_target_phase(&ack_phase, ack_cycles, 1U);
    if (ret == ESP_OK) {
        ret = swd_parlio_target_start(&ack_phase, ack_cycles);
    }
    if (ret != ESP_OK) {
        return DAP_TRANSFER_ERROR;
    }

    /* Fill the next phase while the target is driving the ACK. */
    swd_parlio_phase_t data_phase = swd_parlio_new_phase();
    uint64_t write_bits = 0U;
    uint32_t write_final_data = 1U;
    if (is_read) {
        ret = swd_parlio_prepare_target_phase(
            &data_phase, 33U + turnaround, 1U);
    } else {
        const uint32_t value = data != NULL ? *data : 0U;
        write_bits = (uint64_t)value |
                     ((uint64_t)__builtin_parity(value) << 32U);
        write_final_data = (uint32_t)(write_bits >> 32U) & 1U;
        ret = swd_parlio_prepare_host_phase(
            &data_phase, write_bits, 33U, true, false);
    }
    if (ret != ESP_OK) {
        swd_parlio_abort();
        return DAP_TRANSFER_ERROR;
    }

    ret = swd_parlio_target_finish(ack_cycles, 1U, &response);
    if (ret != ESP_OK) {
        return DAP_TRANSFER_ERROR;
    }
    uint32_t ack = (uint32_t)(response >> turnaround) & 0x07U;

    if (ack == DAP_TRANSFER_OK) {
        if (is_read) {
            uint64_t read_bits = 0U;
            ret = swd_parlio_target_start(
                &data_phase, 33U + turnaround);
            if (ret == ESP_OK) {
                ret = swd_parlio_target_finish(
                    33U + turnaround, 1U, &read_bits);
            }
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
            ret = swd_parlio_run_host_phase(
                &data_phase, write_final_data, true, false);
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
