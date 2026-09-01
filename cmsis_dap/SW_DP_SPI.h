#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef CONFIG_ESP_SWD_USE_SPI

typedef struct {
    uint32_t response_low;
    uint32_t ack;
    uint32_t rx_requested_bits;
    uint32_t rx_programmed_bits;
    uint32_t rx_word0;
    uint32_t rx_word1;
    uint32_t post_done_busy_count;
} swd_esp_spi_debug_t;

esp_err_t swd_esp_spi_init(void);
uint32_t swd_esp_spi_actual_clock_hz(void);
uint32_t swd_esp_spi_cs_setup_cycles(void);
uint32_t swd_esp_spi_rx_standard_alignment(void);
void swd_esp_spi_setup(void);
void swd_esp_spi_off(void);
void swd_esp_spi_get_debug(swd_esp_spi_debug_t *debug);
void swd_esp_spi_set_debug_capture(bool enable);

uint32_t swd_esp_spi_clock_level(void);
void swd_esp_spi_set_clock_idle(uint32_t level);
uint32_t swd_esp_spi_data_level(void);
void swd_esp_spi_set_data_idle(uint32_t level);
uint32_t swd_esp_spi_data_in_level(void);

void swd_esp_spi_swj_sequence(uint32_t count, const uint8_t *data);
void swd_esp_spi_swd_sequence(uint32_t info, const uint8_t *swdo,
                              uint8_t *swdi);
uint8_t swd_esp_spi_transfer(uint32_t request, uint32_t *data);

#endif
