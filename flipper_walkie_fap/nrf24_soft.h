#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <furi_hal_gpio.h>

typedef struct {
    const GpioPin* ce;
    const GpioPin* csn;
    const GpioPin* sck;
    const GpioPin* mosi;
    const GpioPin* miso;
    bool listening;
} Nrf24Soft;

bool nrf24_soft_init(Nrf24Soft* radio);
void nrf24_soft_deinit(Nrf24Soft* radio);

bool nrf24_soft_configure_voice_link(
    Nrf24Soft* radio,
    const uint8_t rx_addr[5],
    const uint8_t tx_addr[5],
    uint8_t channel);

void nrf24_soft_start_listening(Nrf24Soft* radio);
void nrf24_soft_stop_listening(Nrf24Soft* radio);
bool nrf24_soft_data_ready(Nrf24Soft* radio);
bool nrf24_soft_read_payload(Nrf24Soft* radio, void* data, uint8_t size);
bool nrf24_soft_write_payload(Nrf24Soft* radio, const void* data, uint8_t size);
