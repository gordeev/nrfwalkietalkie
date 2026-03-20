#include "nrf24_soft.h"

#include <furi.h>

enum {
    NrfCmdRRegister = 0x00,
    NrfCmdWRegister = 0x20,
    NrfCmdRPayload = 0x61,
    NrfCmdWPayload = 0xA0,
    NrfCmdFlushTx = 0xE1,
    NrfCmdFlushRx = 0xE2,
    NrfCmdNop = 0xFF,
};

enum {
    NrfRegConfig = 0x00,
    NrfRegEnAa = 0x01,
    NrfRegEnRxaddr = 0x02,
    NrfRegSetupAw = 0x03,
    NrfRegSetupRetr = 0x04,
    NrfRegRfCh = 0x05,
    NrfRegRfSetup = 0x06,
    NrfRegStatus = 0x07,
    NrfRegRxAddrP1 = 0x0B,
    NrfRegTxAddr = 0x10,
    NrfRegRxPwP1 = 0x12,
    NrfRegFifoStatus = 0x17,
    NrfRegDynpd = 0x1C,
    NrfRegFeature = 0x1D,
};

enum {
    NrfConfigPrimRx = 0x01,
    NrfConfigPwrUp = 0x02,
    NrfConfigEnCrc = 0x08,
};

enum {
    NrfStatusRxDr = 0x40,
    NrfStatusTxDs = 0x20,
    NrfStatusMaxRt = 0x10,
};

enum {
    NrfFifoRxEmpty = 0x01,
};

static inline void nrf_delay_edge(void) {
    furi_delay_us(1);
}

static void nrf_select(Nrf24Soft* radio) {
    furi_hal_gpio_write(radio->csn, false);
    nrf_delay_edge();
}

static void nrf_deselect(Nrf24Soft* radio) {
    nrf_delay_edge();
    furi_hal_gpio_write(radio->csn, true);
    nrf_delay_edge();
}

static uint8_t nrf_spi_transfer(Nrf24Soft* radio, uint8_t value) {
    uint8_t reply = 0;

    for(uint8_t bit = 0; bit < 8; bit++) {
        const bool out = (value & 0x80u) != 0u;
        furi_hal_gpio_write(radio->mosi, out);
        nrf_delay_edge();

        furi_hal_gpio_write(radio->sck, true);
        nrf_delay_edge();

        reply <<= 1;
        if(furi_hal_gpio_read(radio->miso)) {
            reply |= 1u;
        }

        furi_hal_gpio_write(radio->sck, false);
        nrf_delay_edge();

        value <<= 1;
    }

    return reply;
}

static uint8_t nrf_get_status(Nrf24Soft* radio) {
    uint8_t status;
    nrf_select(radio);
    status = nrf_spi_transfer(radio, NrfCmdNop);
    nrf_deselect(radio);
    return status;
}

static uint8_t nrf_read_reg(Nrf24Soft* radio, uint8_t reg) {
    uint8_t value;
    nrf_select(radio);
    (void)nrf_spi_transfer(radio, NrfCmdRRegister | (reg & 0x1Fu));
    value = nrf_spi_transfer(radio, NrfCmdNop);
    nrf_deselect(radio);
    return value;
}

static void nrf_write_reg(Nrf24Soft* radio, uint8_t reg, uint8_t value) {
    nrf_select(radio);
    (void)nrf_spi_transfer(radio, NrfCmdWRegister | (reg & 0x1Fu));
    (void)nrf_spi_transfer(radio, value);
    nrf_deselect(radio);
}

static void nrf_write_buf(Nrf24Soft* radio, uint8_t reg, const uint8_t* data, uint8_t size) {
    nrf_select(radio);
    (void)nrf_spi_transfer(radio, NrfCmdWRegister | (reg & 0x1Fu));
    for(uint8_t i = 0; i < size; i++) {
        (void)nrf_spi_transfer(radio, data[i]);
    }
    nrf_deselect(radio);
}

static void nrf_command(Nrf24Soft* radio, uint8_t cmd) {
    nrf_select(radio);
    (void)nrf_spi_transfer(radio, cmd);
    nrf_deselect(radio);
}

static void nrf_clear_irqs(Nrf24Soft* radio) {
    nrf_write_reg(radio, NrfRegStatus, NrfStatusRxDr | NrfStatusTxDs | NrfStatusMaxRt);
}

bool nrf24_soft_init(Nrf24Soft* radio) {
    furi_assert(radio);

    furi_hal_gpio_init(radio->ce, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_init(radio->csn, GpioModeOutputPushPull, GpioPullUp, GpioSpeedVeryHigh);
    furi_hal_gpio_init(radio->sck, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_init(radio->mosi, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_init(radio->miso, GpioModeInput, GpioPullUp, GpioSpeedVeryHigh);

    furi_hal_gpio_write(radio->ce, false);
    furi_hal_gpio_write(radio->csn, true);
    furi_hal_gpio_write(radio->sck, false);
    furi_hal_gpio_write(radio->mosi, false);
    radio->listening = false;

    furi_delay_ms(5);

    return nrf_read_reg(radio, NrfRegSetupAw) == 0x03u;
}

void nrf24_soft_deinit(Nrf24Soft* radio) {
    if(!radio) return;

    furi_hal_gpio_write(radio->ce, false);
    furi_hal_gpio_write(radio->csn, true);
    furi_hal_gpio_write(radio->sck, false);
    furi_hal_gpio_write(radio->mosi, false);
}

bool nrf24_soft_configure_voice_link(
    Nrf24Soft* radio,
    const uint8_t rx_addr[5],
    const uint8_t tx_addr[5],
    uint8_t channel) {
    furi_assert(radio);

    furi_hal_gpio_write(radio->ce, false);
    furi_delay_ms(5);

    nrf_write_reg(radio, NrfRegConfig, NrfConfigEnCrc | NrfConfigPwrUp);
    nrf_write_reg(radio, NrfRegEnAa, 0x00);
    nrf_write_reg(radio, NrfRegEnRxaddr, 0x02);
    nrf_write_reg(radio, NrfRegSetupAw, 0x03);
    nrf_write_reg(radio, NrfRegSetupRetr, 0x00);
    nrf_write_reg(radio, NrfRegRfCh, channel);
    nrf_write_reg(radio, NrfRegRfSetup, 0x26);
    nrf_write_buf(radio, NrfRegRxAddrP1, rx_addr, 5);
    nrf_write_buf(radio, NrfRegTxAddr, tx_addr, 5);
    nrf_write_reg(radio, NrfRegRxPwP1, 26);
    nrf_write_reg(radio, NrfRegDynpd, 0x00);
    nrf_write_reg(radio, NrfRegFeature, 0x00);
    nrf_clear_irqs(radio);
    nrf_command(radio, NrfCmdFlushTx);
    nrf_command(radio, NrfCmdFlushRx);

    furi_delay_ms(2);

    return nrf_read_reg(radio, NrfRegRfCh) == channel;
}

void nrf24_soft_start_listening(Nrf24Soft* radio) {
    furi_assert(radio);

    furi_hal_gpio_write(radio->ce, false);
    nrf_write_reg(radio, NrfRegConfig, NrfConfigEnCrc | NrfConfigPwrUp | NrfConfigPrimRx);
    nrf_clear_irqs(radio);
    nrf_command(radio, NrfCmdFlushRx);
    furi_delay_us(150);
    furi_hal_gpio_write(radio->ce, true);
    radio->listening = true;
}

void nrf24_soft_stop_listening(Nrf24Soft* radio) {
    furi_assert(radio);

    furi_hal_gpio_write(radio->ce, false);
    nrf_write_reg(radio, NrfRegConfig, NrfConfigEnCrc | NrfConfigPwrUp);
    nrf_clear_irqs(radio);
    furi_delay_us(150);
    radio->listening = false;
}

bool nrf24_soft_data_ready(Nrf24Soft* radio) {
    return (nrf_read_reg(radio, NrfRegFifoStatus) & NrfFifoRxEmpty) == 0u;
}

bool nrf24_soft_read_payload(Nrf24Soft* radio, void* data, uint8_t size) {
    uint8_t* bytes = data;

    if(size == 0u || size > 32u) return false;

    nrf_select(radio);
    (void)nrf_spi_transfer(radio, NrfCmdRPayload);
    for(uint8_t i = 0; i < size; i++) {
        bytes[i] = nrf_spi_transfer(radio, NrfCmdNop);
    }
    nrf_deselect(radio);

    nrf_write_reg(radio, NrfRegStatus, NrfStatusRxDr);
    return true;
}

bool nrf24_soft_write_payload(Nrf24Soft* radio, const void* data, uint8_t size) {
    const uint8_t* bytes = data;

    if(size == 0u || size > 32u) return false;

    nrf_command(radio, NrfCmdFlushTx);
    nrf_clear_irqs(radio);

    nrf_select(radio);
    (void)nrf_spi_transfer(radio, NrfCmdWPayload);
    for(uint8_t i = 0; i < size; i++) {
        (void)nrf_spi_transfer(radio, bytes[i]);
    }
    nrf_deselect(radio);

    furi_hal_gpio_write(radio->ce, true);
    furi_delay_us(20);
    furi_hal_gpio_write(radio->ce, false);

    for(uint32_t i = 0; i < 2500u; i++) {
        const uint8_t status = nrf_get_status(radio);
        if(status & NrfStatusTxDs) {
            nrf_write_reg(radio, NrfRegStatus, NrfStatusTxDs);
            return true;
        }
        if(status & NrfStatusMaxRt) {
            nrf_write_reg(radio, NrfRegStatus, NrfStatusMaxRt);
            nrf_command(radio, NrfCmdFlushTx);
            return false;
        }
        furi_delay_us(10);
    }

    nrf_command(radio, NrfCmdFlushTx);
    return false;
}
