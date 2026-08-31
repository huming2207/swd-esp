#pragma once

#include <stdint.h>

#include <esp_err.h>

#ifdef CONFIG_ESP_SWD_USE_PARLIO

esp_err_t swd_esp_parlio_init(void);
void swd_esp_parlio_setup(void);
void swd_esp_parlio_off(void);

uint32_t swd_esp_parlio_clock_level(void);
void swd_esp_parlio_set_clock_idle(uint32_t level);
uint32_t swd_esp_parlio_data_level(void);
void swd_esp_parlio_set_data_idle(uint32_t level);
uint32_t swd_esp_parlio_data_in_level(void);

void swd_esp_parlio_swj_sequence(uint32_t count, const uint8_t *data);
void swd_esp_parlio_swd_sequence(uint32_t info, const uint8_t *swdo,
                                 uint8_t *swdi);
uint8_t swd_esp_parlio_transfer(uint32_t request, uint32_t *data);

#endif
