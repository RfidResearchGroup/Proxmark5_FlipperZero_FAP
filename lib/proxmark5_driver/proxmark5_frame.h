#ifndef PROXMARK5_FRAME_H
#define PROXMARK5_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pm3_cmd.h"

void proxmark5_frame_init(void);
void proxmark5_frame_deinit(void);
void proxmark5_frame_reset(void);

// A worker owns the link through the last reply in its operation.
// A cancelled acquisition returns false without taking ownership.
bool proxmark5_frame_begin_operation(const volatile bool* cancel);
bool proxmark5_frame_operation_cancelled(void);
// The operation owner can suspend cancellation while cleaning up.
const volatile bool* proxmark5_frame_set_operation_cancel(const volatile bool* cancel);
void proxmark5_frame_end_operation(void);
bool proxmark5_frame_synchronize(void);
bool proxmark5_frame_echo(uint32_t* rtt_ms);

// Parse packet, print debug packets, and cache non-print responses.
// Returns true when packet was successfully consumed.
bool proxmark5_frame_handle_packet(uint8_t* packet, size_t packet_len);

// Pop one cached response. Returns false if cache is empty.
bool proxmark5_frame_pop(PacketResponseNG* packet);

// Public FIFO get interface for cached responses.
bool proxmark5_frame_get_response(PacketResponseNG* packet);

// Take first cached response that matches cmd. Returns false if no matched cmd.
bool proxmark5_frame_take_by_cmd(uint16_t cmd, PacketResponseNG* packet);

void clearCommandBuffer(void);

void SendCommandOLD(
    uint64_t cmd,
    uint64_t arg0,
    uint64_t arg1,
    uint64_t arg2,
    void* data,
    size_t len);
bool SendCommandNG(uint16_t cmd, uint8_t* data, size_t len);

// Capture debug print lines into the caller's buffer until capture_end.
bool proxmark5_frame_capture_start(char* buffer, size_t capacity);
void proxmark5_frame_capture_end(bool* truncated);

void SendCommandMIX(
    uint64_t cmd,
    uint64_t arg0,
    uint64_t arg1,
    uint64_t arg2,
    void* data,
    size_t len);

// Wait for response by command id until timeout_ms expires.
int proxmark5_frame_wait(
    uint16_t cmd,
    PacketResponseNG* packet,
    uint32_t timeout_ms,
    const volatile bool* cancel);
bool WaitForResponseTimeout(uint16_t cmd, PacketResponseNG* packet, uint32_t timeout_ms);

#endif // PROXMARK5_FRAME_H
