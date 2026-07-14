#include <sdkconfig.h>

#ifdef CONFIG_ESP_SWD_USE_SPI

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_attr.h>
#include <esp_err.h>
#include <esp_private/periph_ctrl.h>
#include <hal/gpio_ll.h>
#include <hal/spi_ll.h>
#include <soc/gpio_struct.h>
#include <soc/soc.h>
#include <soc/spi_periph.h>

#include "DAP_config.h"
#include "DAP.h"
#include "SW_DP_SPI.h"

#define SWD_SPI_HOST             SPI2_HOST
#define SWD_SPI_HW               (&GPSPI2)
#define SWD_SPI_CLOCK_HZ         10000000
#define SWD_SPI_MAX_PHASE_BITS   64U
#define SWD_SPI_CS_SETUP_CYCLES  \
  ((uint32_t)(((uint64_t)SWD_SPI_CLOCK_HZ * ESP_SWD_TURNAROUND_GUARD_NS + \
               999999999ULL) / 1000000000ULL))

_Static_assert(CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ == SWD_SPI_CLOCK_HZ,
               "The experimental SPI backend is fixed at 10 MHz");
_Static_assert(SWD_SPI_CS_SETUP_CYCLES <= UINT8_MAX,
               "The translator guard exceeds the SPI CS setup field");

static bool swd_spi_initialized;
static uint32_t swd_spi_data_idle_level = 1U;
static swd_esp_spi_debug_t swd_spi_debug;

static __always_inline void swd_spi_start_and_wait(void)
{
  while (spi_ll_get_running_cmd(SWD_SPI_HW) != 0U) {
  }
  spi_ll_clear_int_stat(SWD_SPI_HW);
  while (spi_ll_usr_is_done(SWD_SPI_HW)) {
  }
  spi_ll_apply_config(SWD_SPI_HW);
  spi_ll_user_start(SWD_SPI_HW);
  while (!spi_ll_usr_is_done(SWD_SPI_HW)) {
  }
  if (spi_ll_get_running_cmd(SWD_SPI_HW) != 0U) {
    swd_spi_debug.post_done_busy_count++;
    while (spi_ll_get_running_cmd(SWD_SPI_HW) != 0U) {
    }
  }
}

static __always_inline void swd_spi_disable_non_data_phases(void)
{
  spi_ll_set_command_bitlen(SWD_SPI_HW, 0);
  spi_ll_set_addr_bitlen(SWD_SPI_HW, 0);
  spi_ll_set_dummy(SWD_SPI_HW, 0);
}

static __always_inline void swd_spi_tx_once(uint64_t bits, uint32_t count)
{
  SWD_SPI_HW->data_buf[0] = (uint32_t)bits;
  if (count > 32U) {
    SWD_SPI_HW->data_buf[1] = (uint32_t)(bits >> 32U);
  }

  swd_spi_disable_non_data_phases();
  spi_ll_enable_miso(SWD_SPI_HW, false);
  spi_ll_enable_mosi(SWD_SPI_HW, true);
  spi_ll_set_mosi_bitlen(SWD_SPI_HW, count);
  swd_spi_start_and_wait();
}

static __always_inline void swd_spi_tx_command_bit(uint32_t bit)
{
  spi_ll_enable_miso(SWD_SPI_HW, false);
  spi_ll_enable_mosi(SWD_SPI_HW, false);
  spi_ll_set_addr_bitlen(SWD_SPI_HW, 0);
  spi_ll_set_dummy(SWD_SPI_HW, 0);
  spi_ll_set_command(SWD_SPI_HW, (uint16_t)(bit & 1U), 1, true);
  spi_ll_set_command_bitlen(SWD_SPI_HW, 1);
  swd_spi_start_and_wait();
}

static __always_inline uint64_t swd_spi_rx_once(uint32_t count)
{
  swd_spi_disable_non_data_phases();
  spi_ll_enable_mosi(SWD_SPI_HW, false);
  spi_ll_enable_miso(SWD_SPI_HW, true);
  spi_ll_set_miso_bitlen(SWD_SPI_HW, count);
  swd_spi_start_and_wait();

  uint64_t bits = SWD_SPI_HW->data_buf[0];
  if (count > 32U) {
    bits |= (uint64_t)SWD_SPI_HW->data_buf[1] << 32U;
  }
  return bits;
}

uint32_t IRAM_ATTR swd_esp_spi_clock_level(void)
{
  return 0U;
}

void IRAM_ATTR swd_esp_spi_set_clock_idle(uint32_t level)
{
  /* SPI mode 0 keeps SWCLK low between every hardware transaction. */
  (void)level;
}

uint32_t IRAM_ATTR swd_esp_spi_data_level(void)
{
  return swd_spi_data_idle_level;
}

void IRAM_ATTR swd_esp_spi_set_data_idle(uint32_t level)
{
  level &= 1U;
  if (swd_spi_data_idle_level == level) {
    return;
  }

  spi_ll_set_data_pin_idle_level(SWD_SPI_HW, level != 0U);
  spi_ll_apply_config(SWD_SPI_HW);
  swd_spi_data_idle_level = level;
}

uint32_t IRAM_ATTR swd_esp_spi_data_in_level(void)
{
  return gpio_ll_get_level(&GPIO, CONFIG_ESP_SWD_DATA_IN_PIN);
}

static void IRAM_ATTR swd_spi_tx_bits(uint64_t bits, uint32_t count)
{
  if ((count == 0U) || (count > SWD_SPI_MAX_PHASE_BITS)) {
    return;
  }

  const uint32_t final_level = (uint32_t)(bits >> (count - 1U)) & 1U;
  swd_esp_spi_set_data_idle((uint32_t)bits & 1U);

  if ((count & 7U) != 1U) {
    swd_spi_tx_once(bits, count);
  } else if (count == 1U) {
    swd_spi_tx_command_bit((uint32_t)bits);
  } else {
    swd_spi_tx_once(bits, 2U);
    bits >>= 2U;
    swd_esp_spi_set_data_idle((uint32_t)bits & 1U);
    swd_spi_tx_once(bits, count - 2U);
  }

  swd_esp_spi_set_data_idle(final_level);
}

static uint64_t IRAM_ATTR swd_spi_rx_bits(uint32_t count)
{
  if ((count == 0U) || (count > SWD_SPI_MAX_PHASE_BITS)) {
    return 0U;
  }
  return swd_spi_rx_once(count);
}

static void IRAM_ATTR swd_spi_clock_target(uint32_t count)
{
  while (count != 0U) {
    const uint32_t chunk = count > SWD_SPI_MAX_PHASE_BITS
                               ? SWD_SPI_MAX_PHASE_BITS
                               : count;
    (void)swd_spi_rx_bits(chunk);
    count -= chunk;
  }
}

static void IRAM_ATTR swd_spi_clock_host_zero(uint32_t count)
{
  while (count != 0U) {
    const uint32_t chunk = count > SWD_SPI_MAX_PHASE_BITS
                               ? SWD_SPI_MAX_PHASE_BITS
                               : count;
    swd_spi_tx_bits(0U, chunk);
    count -= chunk;
  }
}

static uint64_t IRAM_ATTR swd_spi_load_bits(uint32_t bit_offset,
                                            uint32_t count,
                                            const uint8_t *data)
{
  uint64_t bits = 0U;
  for (uint32_t i = 0U; i < count; i++) {
    const uint32_t source_bit = bit_offset + i;
    const uint32_t bit =
        (data[source_bit >> 3U] >> (source_bit & 7U)) & 1U;
    bits |= (uint64_t)bit << i;
  }
  return bits;
}

esp_err_t swd_esp_spi_init(void)
{
  if (swd_spi_initialized) {
    return ESP_OK;
  }

  PERIPH_RCC_ATOMIC() {
    spi_ll_enable_bus_clock(SWD_SPI_HOST, true);
    spi_ll_reset_register(SWD_SPI_HOST);
  }
  spi_ll_enable_clock(SWD_SPI_HOST, true);
  spi_ll_master_init(SWD_SPI_HW);
  spi_ll_set_clk_source(SWD_SPI_HW, SPI_CLK_SRC_APB);

  spi_ll_clock_val_t clock_reg;
  const int actual_clock = spi_ll_master_cal_clock(
      APB_CLK_FREQ, SWD_SPI_CLOCK_HZ, 128, &clock_reg);
  if (actual_clock != SWD_SPI_CLOCK_HZ) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  spi_ll_master_set_clock_by_reg(SWD_SPI_HW, &clock_reg);
  spi_ll_master_set_mode(SWD_SPI_HW, 0U);
  spi_ll_set_half_duplex(SWD_SPI_HW, true);
  spi_ll_set_sio_mode(SWD_SPI_HW, false);
  spi_ll_set_tx_lsbfirst(SWD_SPI_HW, true);
  spi_ll_set_rx_lsbfirst(SWD_SPI_HW, true);
  spi_ll_master_set_pos_cs(SWD_SPI_HW, 0, false);
  spi_ll_master_select_cs(SWD_SPI_HW, -1);
  spi_ll_master_keep_cs(SWD_SPI_HW, false);
  spi_ll_master_set_cs_setup(SWD_SPI_HW, SWD_SPI_CS_SETUP_CYCLES);
  spi_ll_master_set_cs_hold(SWD_SPI_HW, 0U);
  spi_ll_set_dummy(SWD_SPI_HW, 0);
  spi_ll_enable_mosi(SWD_SPI_HW, false);
  spi_ll_enable_miso(SWD_SPI_HW, false);
  spi_ll_set_data_pin_idle_level(SWD_SPI_HW, true);
  spi_ll_disable_int(SWD_SPI_HW);
  spi_ll_clear_int_stat(SWD_SPI_HW);
  spi_ll_apply_config(SWD_SPI_HW);

  const spi_signal_conn_t *signals = &spi_periph_signal[SWD_SPI_HOST];
  gpio_ll_set_output_signal_matrix_source(
      &GPIO, CONFIG_ESP_SWD_CLK_PIN, signals->spiclk_out, false);
  gpio_ll_set_output_enable_ctrl(
      &GPIO, CONFIG_ESP_SWD_CLK_PIN, false, false);
  gpio_ll_set_output_signal_matrix_source(
      &GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN, signals->spid_out, false);
  gpio_ll_set_output_enable_ctrl(
      &GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN, false, false);
  gpio_ll_set_input_signal_matrix_source(
      &GPIO, signals->spiq_in, CONFIG_ESP_SWD_DATA_IN_PIN, false);
  gpio_ll_set_output_signal_matrix_source(
      &GPIO, CONFIG_ESP_SWD_DATA_NOE_PIN, signals->spics_out[0], false);
  gpio_ll_set_output_enable_ctrl(
      &GPIO, CONFIG_ESP_SWD_DATA_NOE_PIN, false, false);

  swd_spi_data_idle_level = 1U;
  swd_spi_initialized = true;
  return ESP_OK;
}

void IRAM_ATTR swd_esp_spi_setup(void)
{
  spi_ll_master_select_cs(SWD_SPI_HW, 0);
  spi_ll_apply_config(SWD_SPI_HW);
}

void IRAM_ATTR swd_esp_spi_off(void)
{
  if (!swd_spi_initialized) {
    return;
  }
  swd_esp_spi_set_data_idle(1U);
  spi_ll_master_select_cs(SWD_SPI_HW, -1);
  spi_ll_apply_config(SWD_SPI_HW);
}

void swd_esp_spi_get_debug(swd_esp_spi_debug_t *debug)
{
  if (debug != NULL) {
    *debug = swd_spi_debug;
  }
}

void IRAM_ATTR swd_esp_spi_swj_sequence(uint32_t count, const uint8_t *data)
{
  uint32_t offset = 0U;
  while (count != 0U) {
    const uint32_t chunk = count > SWD_SPI_MAX_PHASE_BITS
                               ? SWD_SPI_MAX_PHASE_BITS
                               : count;
    swd_spi_tx_bits(swd_spi_load_bits(offset, chunk, data), chunk);
    offset += chunk;
    count -= chunk;
  }
}

void IRAM_ATTR swd_esp_spi_swd_sequence(uint32_t info, const uint8_t *swdo,
                                       uint8_t *swdi)
{
  uint32_t count = info & SWD_SEQUENCE_CLK;
  if (count == 0U) {
    count = 64U;
  }

  if ((info & SWD_SEQUENCE_DIN) == 0U) {
    swd_esp_spi_swj_sequence(count, swdo);
    return;
  }

  while (count != 0U) {
    const uint32_t chunk = count > SWD_SPI_MAX_PHASE_BITS
                               ? SWD_SPI_MAX_PHASE_BITS
                               : count;
    const uint64_t bits = swd_spi_rx_bits(chunk);
    const uint32_t bytes = (chunk + 7U) / 8U;
    for (uint32_t i = 0U; i < bytes; i++) {
      *swdi++ = (uint8_t)(bits >> (8U * i));
    }
    count -= chunk;
  }
}

uint8_t IRAM_ATTR swd_esp_spi_transfer(uint32_t request, uint32_t *data)
{
  swd_spi_debug = (swd_esp_spi_debug_t) {};
  const bool is_read = (request & DAP_TRANSFER_RnW) != 0U;
  const uint32_t request_bits = request & 0x0FU;
  const uint32_t packet = 1U | (request_bits << 1U) |
      ((uint32_t)__builtin_parity(request_bits) << 5U) | (1U << 7U);

  swd_esp_spi_set_data_idle(1U);
  swd_spi_tx_bits(packet, 8U);

  swd_esp_swdio_target_drive();
  const uint32_t turnaround = DAP_Data.swd_conf.turnaround;
  const uint64_t response = swd_spi_rx_bits(turnaround + 3U);

  /*
   * ESP32-S3 GPSPI samples MISO half a clock after the standard SPI edge.
   * The turnaround window therefore captures ACK[0:2] one position earlier
   * than rising-edge SWD framing, followed by RDATA[0] for a successful read.
   */
  const uint32_t ack_shift = turnaround - 1U;
  uint32_t ack = (uint32_t)(response >> ack_shift) & 0x07U;
  const uint32_t first_read_bit =
      (uint32_t)(response >> (ack_shift + 3U)) & 1U;
  swd_spi_debug.ack_bits = (uint32_t)response;
  swd_spi_debug.ack = ack;

  if (ack == DAP_TRANSFER_OK) {
    if (is_read) {
      swd_spi_debug.read_requested_bits = 33U;
      const uint64_t read_tail = swd_spi_rx_bits(33U);
      swd_spi_debug.read_programmed_bits =
          SWD_SPI_HW->ms_dlen.ms_data_bitlen + 1U;
      swd_spi_debug.read_word0 = SWD_SPI_HW->data_buf[0];
      swd_spi_debug.read_word1 = SWD_SPI_HW->data_buf[1];
      const uint32_t value = first_read_bit | ((uint32_t)read_tail << 1U);
      const uint32_t parity = (uint32_t)(read_tail >> 31U) & 1U;
      if (((uint32_t)__builtin_parity(value) ^ parity) != 0U) {
        ack = DAP_TRANSFER_ERROR;
      }
      if (data != NULL) {
        *data = value;
      }
      swd_spi_clock_target(DAP_Data.swd_conf.turnaround);
      swd_esp_swdio_host_drive(1U);
    } else {
      const uint32_t value = *data;
      const uint64_t write_bits =
          (uint64_t)value | ((uint64_t)__builtin_parity(value) << 32U);
      swd_spi_clock_target(DAP_Data.swd_conf.turnaround);
      swd_esp_swdio_host_drive(value & 1U);
      swd_spi_tx_bits(write_bits, 33U);
    }

    if (request & DAP_TRANSFER_TIMESTAMP) {
      DAP_Data.timestamp = TIMESTAMP_GET();
    }
    if (DAP_Data.transfer.idle_cycles != 0U) {
      swd_spi_clock_host_zero(DAP_Data.transfer.idle_cycles);
    }
    swd_esp_spi_set_data_idle(1U);
    return (uint8_t)ack;
  }

  if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
    if (DAP_Data.swd_conf.data_phase && is_read) {
      swd_spi_clock_target(33U);
    }
    swd_spi_clock_target(DAP_Data.swd_conf.turnaround);
    if (DAP_Data.swd_conf.data_phase && !is_read) {
      swd_esp_swdio_host_drive(0U);
      swd_spi_clock_host_zero(33U);
    } else {
      swd_esp_swdio_host_drive(1U);
    }
    swd_esp_spi_set_data_idle(1U);
    return (uint8_t)ack;
  }

  swd_spi_clock_target(DAP_Data.swd_conf.turnaround + 33U);
  swd_esp_swdio_host_drive(1U);
  swd_esp_spi_set_data_idle(1U);
  return (uint8_t)ack;
}

#endif
