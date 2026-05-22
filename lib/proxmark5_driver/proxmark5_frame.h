#ifndef PROXMARK5_FRAME_H
#define PROXMARK5_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PM3_CMD_DATA_SIZE 512U

typedef struct {
    uint16_t cmd;
    uint16_t length;
    uint32_t magic;
    int8_t status;
    int8_t reason;
    uint16_t crc;
    uint64_t oldarg[3];
    union {
        uint8_t asBytes[PM3_CMD_DATA_SIZE];
        uint32_t asDwords[PM3_CMD_DATA_SIZE / 4U];
    } data;
    bool ng;
} PacketResponseNG;

void proxmark5_frame_init(void);
void proxmark5_frame_reset(void);

int reply_old(uint64_t cmd, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* data, size_t len);
int reply_ng(uint16_t cmd, int8_t status, uint8_t* data, size_t len);
int reply_mix(uint64_t cmd, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* data, size_t len);
int reply_reason(uint16_t cmd, int8_t status, int8_t reason, uint8_t* data, size_t len);

// Parse packet, print debug packets, and cache non-print responses.
// Returns true when packet was successfully consumed.
bool proxmark5_frame_handle_packet(uint8_t* packet, size_t packet_len);

// Pop one cached response. Returns false if cache is empty.
bool proxmark5_frame_pop(PacketResponseNG* packet);

// Public FIFO get interface for cached responses.
bool proxmark5_frame_get_response(PacketResponseNG* packet);

// Take first cached response that matches cmd. Returns false if no matched cmd.
bool proxmark5_frame_take_by_cmd(uint16_t cmd, PacketResponseNG* packet);

// Wait for response by command id until timeout_ms expires.
bool proxmark5_frame_wait_response(uint16_t cmd, PacketResponseNG* packet, uint32_t timeout_ms);

#endif // PROXMARK5_FRAME_H
