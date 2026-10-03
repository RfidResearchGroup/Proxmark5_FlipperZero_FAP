#include <furi_hal.h>
#include <furi_hal_spi.h>
#include "proxmark5_spi.h"

// SPI bus handle for proxmark5 communication
const FuriHalSpiBusHandle* spi_bus = &furi_hal_spi_bus_handle_external;

// furi_hal_spi_acquire() hard-asserts (furi_check) if the bus is already
// held rather than blocking - it's meant for a single caller per bus. We
// have two independent threads touching this same handle (the RX poll
// thread in proxmark5_com.c, and whichever thread calls SendCommandNG),
// so without this mutex any overlap between them crashes the whole
// Flipper, not just this app. The RX thread holds the bus for most of its
// duty cycle (up to its 100ms header-read timeout every ~20ms idle loop),
// so that overlap is common, not a rare edge case.
static FuriMutex* spi_lock = NULL;

/**
 * @brief Initialize the SPI bus for proxmark5 communication
 *
 */
void proxmark5_spi_init(void) {
    spi_lock = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_hal_spi_bus_handle_init(spi_bus);
}

/**
 * @brief Deinitialize the SPI bus
 *
 */
void proxmark5_spi_deinit(void) {
    furi_hal_spi_bus_handle_deinit(spi_bus);
    furi_mutex_free(spi_lock);
    spi_lock = NULL;
}

/**
 * @brief Send data over SPI to proxmark5, will release the bus after transmission
 *
 * @param data Pointer to the data to send
 * @param length Number of bytes to send
 * @param timeout Timeout in milliseconds
 */
bool proxmark5_spi_send_data(uint8_t* data, size_t length, uint32_t timeout) {
    proxmark5_spi_acquire();
    bool ret = furi_hal_spi_bus_tx(spi_bus, data, length, timeout);
    proxmark5_spi_release();
    return ret;
}

/**
 * @brief Acquire the SPI bus (asserts CS) without performing a transfer.
 * Pair with proxmark5_spi_release() around one or more _raw() calls to keep
 * them inside a single continuous CS-asserted transaction.
 */
void proxmark5_spi_acquire(void) {
    furi_mutex_acquire(spi_lock, FuriWaitForever);
    furi_hal_spi_acquire(spi_bus);
}

/**
 * @brief Release the SPI bus (deasserts CS). See proxmark5_spi_acquire().
 */
void proxmark5_spi_release(void) {
    furi_hal_spi_release(spi_bus);
    furi_mutex_release(spi_lock);
}

/**
 * @brief Transmit without acquiring/releasing the bus - caller must already
 * hold it via proxmark5_spi_acquire().
 */
bool proxmark5_spi_tx_raw(uint8_t* data, size_t length, uint32_t timeout) {
    return furi_hal_spi_bus_tx(spi_bus, data, length, timeout);
}

/**
 * @brief Transceive without acquiring/releasing the bus - caller must
 * already hold it via proxmark5_spi_acquire().
 */
bool proxmark5_spi_trx_raw(uint8_t* buffer, size_t length, uint32_t timeout) {
    return furi_hal_spi_bus_trx(spi_bus, NULL, buffer, length, timeout);
}

/**
 * @brief Receive data over SPI from proxmark5, will release the bus after reception
 * 
 * @param buffer Pointer to the buffer to store received data
 * @param length Number of bytes to receive
 * @param timeout Timeout in milliseconds
 */
bool proxmark5_spi_receive_data(uint8_t* buffer, size_t length, uint32_t timeout) {
    proxmark5_spi_acquire();
    bool ret = furi_hal_spi_bus_trx(spi_bus, NULL, buffer, length, timeout);
    proxmark5_spi_release();
    return ret;
}
