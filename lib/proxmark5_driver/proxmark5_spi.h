#ifndef proxmark5_SPI_H
#define proxmark5_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void proxmark5_spi_init(void);
void proxmark5_spi_deinit(void);
bool proxmark5_spi_send_data(uint8_t* data, size_t length, uint32_t timeout);
bool proxmark5_spi_receive_data(uint8_t* buffer, size_t length, uint32_t timeout);

// Raw primitives: no internal acquire/release, so a caller can bracket
// several of these with one acquire/release pair to keep them in a single
// CS-asserted transaction. PM5's HW-CS slave mode starts a fresh frame on
// every CS edge, so acquiring/releasing per piece desyncs the two sides.
void proxmark5_spi_acquire(void);
void proxmark5_spi_release(void);
bool proxmark5_spi_tx_raw(uint8_t* data, size_t length, uint32_t timeout);
bool proxmark5_spi_trx_raw(uint8_t* buffer, size_t length, uint32_t timeout);

#endif // proxmark5_SPI_H
