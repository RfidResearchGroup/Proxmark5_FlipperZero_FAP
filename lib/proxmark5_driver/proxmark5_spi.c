#include <furi_hal.h>
#include <furi_hal_spi.h>
#include <stdlib.h>
#include <string.h>

// SPI bus handle for proxmark5 communication
const FuriHalSpiBusHandle* spi_bus = &furi_hal_spi_bus_handle_external;

/**
 * @brief Initialize the SPI bus for proxmark5 communication
 * 
 */
void proxmark5_spi_init(void) {
    furi_hal_spi_bus_handle_init(spi_bus);
}

/**
 * @brief Deinitialize the SPI bus
 * 
 */
void proxmark5_spi_deinit(void) {
    furi_hal_spi_bus_handle_deinit(spi_bus);
}

/**
 * @brief Send data over SPI to proxmark5, will release the bus after transmission
 * 
 * @param data Pointer to the data to send
 * @param length Number of bytes to send
 * @param timeout Timeout in milliseconds
 */
bool proxmark5_spi_send_data(uint8_t* data, size_t length, uint32_t timeout) {
    furi_hal_spi_acquire(spi_bus);
    bool ret = furi_hal_spi_bus_tx(spi_bus, data, length, timeout);
    furi_hal_spi_release(spi_bus);
    return ret;
}

bool proxmark5_spi_send_frame(uint8_t* data, size_t length, uint32_t timeout) {
    // Single bus hold for whole frame (avoids PM5 reading length before payload arrives)
    furi_hal_spi_acquire(spi_bus);
    bool ret = furi_hal_spi_bus_tx(spi_bus, data, length, timeout);
    furi_hal_spi_release(spi_bus);
    return ret;
}

bool proxmark5_spi_send_then_receive(
    uint8_t* tx,
    size_t tx_len,
    uint8_t* rx,
    size_t rx_len,
    uint32_t timeout) {
    if(!tx || !rx || tx_len == 0 || rx_len == 0) {
        return false;
    }

    uint8_t dummy_stack[64];
    uint8_t* dummy = dummy_stack;
    uint8_t* dummy_heap = NULL;
    if(rx_len > sizeof(dummy_stack)) {
        dummy_heap = malloc(rx_len);
        dummy = dummy_heap;
        if(!dummy) {
            return false;
        }
    }
    memset(dummy, 0, rx_len);

    // Keep CS low from command through reply — AT32 slave drops TX if CS rises
    furi_hal_spi_acquire(spi_bus);
    bool ok = furi_hal_spi_bus_tx(spi_bus, tx, tx_len, timeout);
    if(ok) {
        furi_delay_ms(80);
        ok = furi_hal_spi_bus_trx(spi_bus, dummy, rx, rx_len, timeout);
    }
    furi_hal_spi_release(spi_bus);
    if(dummy_heap) {
        free(dummy_heap);
    }
    return ok;
}

bool proxmark5_spi_receive_data(uint8_t* buffer, size_t length, uint32_t timeout) {
    // Never pass NULL TX: some HAL builds clock garbage and desync the PM5 slave
    uint8_t dummy_stack[64];
    uint8_t* dummy = dummy_stack;
    uint8_t* dummy_heap = NULL;
    if(length > sizeof(dummy_stack)) {
        dummy_heap = malloc(length);
        dummy = dummy_heap;
        if(!dummy) {
            return false;
        }
    }
    memset(dummy, 0, length);
    furi_hal_spi_acquire(spi_bus);
    bool ret = furi_hal_spi_bus_trx(spi_bus, dummy, buffer, length, timeout);
    furi_hal_spi_release(spi_bus);
    if(dummy_heap) {
        free(dummy_heap);
    }
    return ret;
}

bool proxmark5_spi_receive_packet(
    uint8_t* buffer,
    size_t max_len,
    uint16_t* out_len,
    uint32_t timeout) {
    if(!buffer || !out_len || max_len == 0) {
        return false;
    }

    uint8_t tx_hdr[2] = {0, 0};
    uint8_t rx_hdr[2] = {0, 0};
    uint8_t dummy_stack[64];
    uint8_t* dummy = dummy_stack;
    uint8_t* dummy_heap = NULL;

    furi_hal_spi_acquire(spi_bus);
    bool ok = furi_hal_spi_bus_trx(spi_bus, tx_hdr, rx_hdr, 2, timeout);
    if(!ok) {
        furi_hal_spi_release(spi_bus);
        return false;
    }

    uint16_t n = (uint16_t)rx_hdr[0] | ((uint16_t)rx_hdr[1] << 8);
    if(n == 0 || n > max_len) {
        furi_hal_spi_release(spi_bus);
        return false;
    }

    if(n > sizeof(dummy_stack)) {
        dummy_heap = malloc(n);
        dummy = dummy_heap;
        if(!dummy) {
            furi_hal_spi_release(spi_bus);
            return false;
        }
    }
    memset(dummy, 0, n);
    ok = furi_hal_spi_bus_trx(spi_bus, dummy, buffer, n, timeout);
    furi_hal_spi_release(spi_bus);
    if(dummy_heap) {
        free(dummy_heap);
    }
    if(!ok) {
        return false;
    }
    *out_len = n;
    return true;
}
