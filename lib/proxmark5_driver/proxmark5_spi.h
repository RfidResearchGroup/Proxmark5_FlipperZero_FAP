#ifndef proxmark5_SPI_H
#define proxmark5_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void proxmark5_spi_init(void);
void proxmark5_spi_deinit(void);
bool proxmark5_spi_send_data(uint8_t* data, size_t length, uint32_t timeout);
// Hold CS/bus for the whole frame (len hdr + payload + trailer)
bool proxmark5_spi_send_frame(uint8_t* data, size_t length, uint32_t timeout);
bool proxmark5_spi_send_then_receive(
    uint8_t* tx,
    size_t tx_len,
    uint8_t* rx,
    size_t rx_len,
    uint32_t timeout);
bool proxmark5_spi_receive_data(uint8_t* buffer, size_t length, uint32_t timeout);
// Read [len][payload] without releasing CS between the two phases
bool proxmark5_spi_receive_packet(
    uint8_t* buffer,
    size_t max_len,
    uint16_t* out_len,
    uint32_t timeout);

#endif // proxmark5_SPI_H
