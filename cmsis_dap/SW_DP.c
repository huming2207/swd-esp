/*
 * Copyright (c) 2013-2017 ARM Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ----------------------------------------------------------------------
 *
 * $Date:        1. December 2017
 * $Revision:    V2.0.0
 *
 * Project:      CMSIS-DAP Source
 * Title:        SW_DP.c CMSIS-DAP SW DP I/O
 *
 *---------------------------------------------------------------------------*/

#include <esp_attr.h>
#include "DAP_config.h"
#include "DAP.h"
#include "swd_perf.h"

// SW Macros

#define PIN_SWCLK_SET PIN_SWCLK_TCK_SET
#define PIN_SWCLK_CLR PIN_SWCLK_TCK_CLR

#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
static DRAM_ATTR swd_perf_counters_t swd_perf_counters = {
  .min_transfer_cycles = UINT32_MAX,
};

static __always_inline void swd_perf_record_drive(bool target, uint32_t cycles)
{
  if (target) {
    swd_perf_counters.target_drive_cycles += cycles;
    swd_perf_counters.target_drive_count++;
  } else {
    swd_perf_counters.host_drive_cycles += cycles;
    swd_perf_counters.host_drive_count++;
  }
}

static __always_inline void swd_perf_record_transfer(uint32_t request, uint8_t ack,
                                                     uint32_t cycles)
{
  swd_perf_counters.transfer_cycles += cycles;
  swd_perf_counters.transfer_count++;
  if (request & DAP_TRANSFER_RnW) {
    swd_perf_counters.read_transfer_cycles += cycles;
    swd_perf_counters.read_transfer_count++;
  } else {
    swd_perf_counters.write_transfer_cycles += cycles;
    swd_perf_counters.write_transfer_count++;
  }

  if (cycles < swd_perf_counters.min_transfer_cycles) {
    swd_perf_counters.min_transfer_cycles = cycles;
  }
  if (cycles > swd_perf_counters.max_transfer_cycles) {
    swd_perf_counters.max_transfer_cycles = cycles;
  }

  switch (ack) {
    case DAP_TRANSFER_OK:
      swd_perf_counters.ack_ok_cycles += cycles;
      swd_perf_counters.ack_ok_count++;
      break;
    case DAP_TRANSFER_WAIT:
      swd_perf_counters.ack_wait_cycles += cycles;
      swd_perf_counters.ack_wait_count++;
      break;
    case DAP_TRANSFER_FAULT:
      swd_perf_counters.ack_fault_cycles += cycles;
      swd_perf_counters.ack_fault_count++;
      break;
    case DAP_TRANSFER_ERROR:
      swd_perf_counters.ack_error_cycles += cycles;
      swd_perf_counters.ack_error_count++;
      break;
    default:
      swd_perf_counters.ack_invalid_cycles += cycles;
      swd_perf_counters.ack_invalid_count++;
      break;
  }
}
#endif

void IRAM_ATTR swd_perf_record_retry(uint32_t request, uint32_t wait_count,
                                    uint8_t final_ack)
{
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
  const uint32_t request_bucket = request & 0x0FU;
  const uint32_t attempt_count =
      wait_count + (final_ack == DAP_TRANSFER_WAIT ? 0U : 1U);
  uint32_t bucket;

  swd_perf_counters.request_count[request_bucket] += attempt_count;
  swd_perf_counters.request_wait_count[request_bucket] += wait_count;
  if (final_ack == DAP_TRANSFER_OK) {
    swd_perf_counters.request_ok_count[request_bucket]++;
  }
  swd_perf_counters.retry_call_count++;
  swd_perf_counters.retry_wait_count += wait_count;
  if (wait_count > swd_perf_counters.retry_max_waits) {
    swd_perf_counters.retry_max_waits = wait_count;
  }

  if (wait_count <= 3U) {
    bucket = wait_count;
  } else if (wait_count <= 7U) {
    bucket = 4U;
  } else {
    bucket = 5U;
  }
  swd_perf_counters.retry_wait_streaks[bucket]++;
#else
  (void)request;
  (void)wait_count;
  (void)final_ack;
#endif
}

void swd_perf_reset_counters(void)
{
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
  swd_perf_counters = (swd_perf_counters_t) {
    .min_transfer_cycles = UINT32_MAX,
  };
#endif
}

void swd_perf_get_counters(swd_perf_counters_t *counters)
{
  if (counters == NULL) {
    return;
  }

#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
  *counters = swd_perf_counters;
  if (counters->transfer_count == 0U) {
    counters->min_transfer_cycles = 0U;
  }
#else
  *counters = (swd_perf_counters_t) {};
#endif
}

#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
static __always_inline void swd_esp_turnaround_guard(void)
{
#if (CONFIG_ESP_SWD_TURNAROUND_DELAY_US > 0) || (CONFIG_ESP_SWD_TURNAROUND_DELAY_NS > 0)
  PIN_DELAY_SLOW(ESP_SWD_TURNAROUND_GUARD_CYCLES);
#endif
}

void IRAM_ATTR swd_esp_swdio_target_drive(void)
{
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
  const uint32_t started = esp_cpu_get_cycle_count();
#endif
  /* Begin turnaround with SWCLK low and isolate the translator before DIR. */
  PIN_SWCLK_CLR();
  swd_esp_translator_set_noe(1U);
  gpio_ll_output_disable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
  gpio_ll_input_enable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
  swd_esp_turnaround_guard();
  swd_esp_translator_set_direction(0U);
#ifndef CONFIG_ESP_SWD_USE_SPI
  swd_esp_translator_set_noe(0U);
  swd_esp_turnaround_guard();
#endif
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
  swd_perf_record_drive(true, esp_cpu_get_cycle_count() - started);
#endif
}

void IRAM_ATTR swd_esp_swdio_host_drive(uint32_t initial_bit)
{
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
  const uint32_t started = esp_cpu_get_cycle_count();
#endif
  /* Preload the first value while the translator and ESP output are isolated. */
  PIN_SWCLK_CLR();
  swd_esp_translator_set_noe(1U);
  swd_esp_turnaround_guard();
  swd_esp_translator_set_direction(1U);
  PIN_SWDIO_OUT(initial_bit);
  gpio_ll_input_disable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
  gpio_ll_output_enable(&GPIO, CONFIG_ESP_SWD_DATA_OUT_PIN);
#ifndef CONFIG_ESP_SWD_USE_SPI
  swd_esp_translator_set_noe(0U);
  swd_esp_turnaround_guard();
#endif
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
  swd_perf_record_drive(false, esp_cpu_get_cycle_count() - started);
#endif
}
#endif

#define SW_CLOCK_CYCLE()                \
  PIN_SWCLK_CLR();                      \
  PIN_DELAY();                          \
  PIN_SWCLK_SET();                      \
  PIN_DELAY()

#define SW_WRITE_BIT(bit)               \
  PIN_SWDIO_OUT(bit);                   \
  PIN_SWCLK_CLR();                      \
  PIN_DELAY();                          \
  PIN_SWCLK_SET();                      \
  PIN_DELAY()

#define SW_READ_BIT(bit)                \
  PIN_SWCLK_CLR();                      \
  PIN_DELAY();                          \
  bit = PIN_SWDIO_IN();                 \
  PIN_SWCLK_SET();                      \
  PIN_DELAY()

#define PIN_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)


// Generate SWJ Sequence
//   count:  sequence bit count
//   data:   pointer to sequence bit data
//   return: none
#if ((DAP_SWD != 0) || (DAP_JTAG != 0))
void IRAM_ATTR SWJ_Sequence (uint32_t count, const uint8_t *data) {
#ifdef CONFIG_ESP_SWD_USE_SPI
  swd_esp_spi_swj_sequence(count, data);
#else
  uint32_t val;
  uint32_t n;

  val = 0U;
  n = 0U;
  while (count--) {
    if (n == 0U) {
      val = *data++;
      n = 8U;
    }
    if (val & 1U) {
      PIN_SWDIO_TMS_SET();
    } else {
      PIN_SWDIO_TMS_CLR();
    }
    SW_CLOCK_CYCLE();
    val >>= 1;
    n--;
  }
#endif
}
#endif


// Generate SWD Sequence
//   info:   sequence information
//   swdo:   pointer to SWDIO generated data
//   swdi:   pointer to SWDIO captured data
//   return: none
#if (DAP_SWD != 0)
void IRAM_ATTR SWD_Sequence (uint32_t info, const uint8_t *swdo, uint8_t *swdi) {
#ifdef CONFIG_ESP_SWD_USE_SPI
  swd_esp_spi_swd_sequence(info, swdo, swdi);
#else
  uint32_t val;
  uint32_t bit;
  uint32_t n, k;

  n = info & SWD_SEQUENCE_CLK;
  if (n == 0U) {
    n = 64U;
  }

  if (info & SWD_SEQUENCE_DIN) {
    while (n) {
      val = 0U;
      for (k = 8U; k && n; k--, n--) {
        SW_READ_BIT(bit);
        val >>= 1;
        val  |= bit << 7;
      }
      val >>= k;
      *swdi++ = (uint8_t)val;
    }
  } else {
    while (n) {
      val = *swdo++;
      for (k = 8U; k && n; k--, n--) {
        SW_WRITE_BIT(val);
        val >>= 1;
      }
    }
  }
#endif
}
#endif


#if (DAP_SWD != 0)


// SWD Transfer I/O
//   request: A[3:2] RnW APnDP
//   data:    DATA[31:0]
//   return:  ACK[2:0]
#define SWD_TransferFunction(speed)     /**/                                    \
static inline __attribute__((always_inline)) uint8_t SWD_Transfer##speed (uint32_t request, uint32_t *data) {         \
  uint32_t ack;                                                                 \
  uint32_t bit;                                                                 \
  uint32_t val;                                                                 \
  uint32_t parity;                                                              \
                                                                                \
  uint32_t n;                                                                   \
                                                                                \
  /* Packet Request */                                                          \
  parity = 0U;                                                                  \
  SW_WRITE_BIT(1U);                     /* Start Bit */                         \
  bit = request >> 0;                                                           \
  SW_WRITE_BIT(bit);                    /* APnDP Bit */                         \
  parity += bit;                                                                \
  bit = request >> 1;                                                           \
  SW_WRITE_BIT(bit);                    /* RnW Bit */                           \
  parity += bit;                                                                \
  bit = request >> 2;                                                           \
  SW_WRITE_BIT(bit);                    /* A2 Bit */                            \
  parity += bit;                                                                \
  bit = request >> 3;                                                           \
  SW_WRITE_BIT(bit);                    /* A3 Bit */                            \
  parity += bit;                                                                \
  SW_WRITE_BIT(parity);                 /* Parity Bit */                        \
  SW_WRITE_BIT(0U);                     /* Stop Bit */                          \
  SW_WRITE_BIT(1U);                     /* Park Bit */                          \
                                                                                \
  /* Turnaround */                                                              \
  PIN_SWDIO_OUT_DISABLE();                                                      \
  for (n = DAP_Data.swd_conf.turnaround; n; n--) {                              \
    SW_CLOCK_CYCLE();                                                           \
  }                                                                             \
                                                                                \
  /* Acknowledge response */                                                    \
  SW_READ_BIT(bit);                                                             \
  ack  = bit << 0;                                                              \
  SW_READ_BIT(bit);                                                             \
  ack |= bit << 1;                                                              \
  SW_READ_BIT(bit);                                                             \
  ack |= bit << 2;                                                              \
                                                                                \
  if (ack == DAP_TRANSFER_OK) {         /* OK response */                       \
    /* Data transfer */                                                         \
    if (request & DAP_TRANSFER_RnW) {                                           \
      /* Read data */                                                           \
      val = 0U;                                                                 \
      parity = 0U;                                                              \
      for (n = 32U; n; n--) {                                                   \
        SW_READ_BIT(bit);               /* Read RDATA[0:31] */                  \
        parity += bit;                                                          \
        val >>= 1;                                                              \
        val  |= bit << 31;                                                      \
      }                                                                         \
      SW_READ_BIT(bit);                 /* Read Parity */                       \
      if ((parity ^ bit) & 1U) {                                                \
        ack = DAP_TRANSFER_ERROR;                                               \
      }                                                                         \
      if (data) { *data = val; }                                                \
      /* Turnaround */                                                          \
      for (n = DAP_Data.swd_conf.turnaround; n; n--) {                          \
        SW_CLOCK_CYCLE();                                                       \
      }                                                                         \
      PIN_SWDIO_OUT_ENABLE();                                                   \
    } else {                                                                    \
      /* Turnaround */                                                          \
      val = *data;                                                              \
      for (n = DAP_Data.swd_conf.turnaround; n; n--) {                          \
        SW_CLOCK_CYCLE();                                                       \
      }                                                                         \
      PIN_SWDIO_OUT_ENABLE_VALUE(val);                                          \
      /* Write data */                                                          \
      parity = 0U;                                                              \
      for (n = 32U; n; n--) {                                                   \
        SW_WRITE_BIT(val);              /* Write WDATA[0:31] */                 \
        parity += val;                                                          \
        val >>= 1;                                                              \
      }                                                                         \
      SW_WRITE_BIT(parity);             /* Write Parity Bit */                  \
    }                                                                           \
    /* Capture Timestamp */                                                     \
    if (request & DAP_TRANSFER_TIMESTAMP) {                                     \
      DAP_Data.timestamp = TIMESTAMP_GET();                                     \
    }                                                                           \
    /* Idle cycles */                                                           \
    n = DAP_Data.transfer.idle_cycles;                                          \
    if (n) {                                                                    \
      PIN_SWDIO_OUT(0U);                                                        \
      for (; n; n--) {                                                          \
        SW_CLOCK_CYCLE();                                                       \
      }                                                                         \
    }                                                                           \
    PIN_SWDIO_OUT(1U);                                                          \
    return ((uint8_t)ack);                                                      \
  }                                                                             \
                                                                                \
  if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {              \
    /* WAIT or FAULT response */                                                \
    if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) { \
      for (n = 32U+1U; n; n--) {                                                \
        SW_CLOCK_CYCLE();               /* Dummy Read RDATA[0:31] + Parity */   \
      }                                                                         \
    }                                                                           \
    /* Turnaround */                                                            \
    for (n = DAP_Data.swd_conf.turnaround; n; n--) {                            \
      SW_CLOCK_CYCLE();                                                         \
    }                                                                           \
    if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) { \
      PIN_SWDIO_OUT_ENABLE_VALUE(0U);                                           \
      for (n = 32U+1U; n; n--) {                                                \
        SW_CLOCK_CYCLE();               /* Dummy Write WDATA[0:31] + Parity */  \
      }                                                                         \
    } else {                                                                    \
      PIN_SWDIO_OUT_ENABLE_VALUE(1U);                                           \
    }                                                                           \
    PIN_SWDIO_OUT(1U);                                                          \
    return ((uint8_t)ack);                                                      \
  }                                                                             \
                                                                                \
  /* Protocol error */                                                          \
  for (n = DAP_Data.swd_conf.turnaround + 32U + 1U; n; n--) {                   \
    SW_CLOCK_CYCLE();                   /* Back off data phase */               \
  }                                                                             \
  PIN_SWDIO_OUT_ENABLE();                                                       \
  PIN_SWDIO_OUT(1U);                                                            \
  return ((uint8_t)ack);                                                        \
}


#undef  PIN_DELAY
#define PIN_DELAY() PIN_DELAY_FAST()
SWD_TransferFunction(Fast)

#undef  PIN_DELAY
#define PIN_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)
SWD_TransferFunction(Slow)


// SWD Transfer I/O
//   request: A[3:2] RnW APnDP
//   data:    DATA[31:0]
//   return:  ACK[2:0]
uint8_t IRAM_ATTR SWD_Transfer(uint32_t request, uint32_t *data) {
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
  const uint32_t started = esp_cpu_get_cycle_count();
#endif
  uint8_t ack;

#ifdef CONFIG_ESP_SWD_USE_SPI
  ack = swd_esp_spi_transfer(request, data);
#else
  if (DAP_Data.fast_clock) {
    ack = SWD_TransferFast(request, data);
  } else {
    ack = SWD_TransferSlow(request, data);
  }
#endif
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
  swd_perf_record_transfer(request, ack, esp_cpu_get_cycle_count() - started);
#endif
  return ack;
}


#endif  /* (DAP_SWD != 0) */
