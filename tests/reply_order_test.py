#!/usr/bin/env python3
# Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

"""Replay late replies through the production frame queue."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
DRIVER = ROOT / "lib/proxmark5_driver"

HAL = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define furi_assert assert
static inline void test_log(const char* tag, const char* message, ...) {
    (void)tag;
    (void)message;
}
#define FURI_LOG_I(...) test_log(__VA_ARGS__)
#define FURI_LOG_W(...) test_log(__VA_ARGS__)
#define FuriMutexTypeNormal 0
#define FuriWaitForever (-1)
#define FuriStatusOk 0
typedef struct { bool held; } FuriMutex;
static inline FuriMutex* furi_mutex_alloc(int type) {
    (void)type;
    return calloc(1, sizeof(FuriMutex));
}
static inline int furi_mutex_acquire(FuriMutex* m, int timeout) {
    if(m->held) {
        assert(timeout == 0);
        return -1;
    }
    m->held = true;
    return FuriStatusOk;
}
static inline void furi_mutex_release(FuriMutex* m) {
    assert(m->held);
    m->held = false;
}
static inline void furi_mutex_free(FuriMutex* m) {
    assert(!m->held);
    free(m);
}
uint32_t furi_get_tick(void);
void furi_delay_ms(uint32_t ms);
'''

HOST = r'''
#include <furi_hal.h>
#include <stdio.h>
#include <string.h>
#include "proxmark5_frame.h"

static uint32_t tick, old_due, new_due, echo_due, stale_due, cancel_due;
static unsigned selects, pings, handshakes;
static bool suppress_echo, link_active, handshake_ok;
static bool delay_first_echo, waiting_for_reply;
static volatile bool canceled;
static uint8_t nonce[12], stale_nonce[12];
static void deliver(uint16_t cmd, const void* payload, uint16_t size) {
    PacketResponseNGRaw frame = {.pre = {
        .magic = RESPONSENG_PREAMBLE_MAGIC, .cmd = cmd, .ng = true, .length = size,
    }};
    const uint16_t trailer = RESPONSENG_POSTAMBLE_MAGIC;
    memcpy(frame.data, payload, size);
    memcpy(frame.data + size, &trailer, sizeof(trailer));
    assert(proxmark5_frame_handle_packet(
        (uint8_t*)&frame, sizeof(frame.pre) + size + sizeof(trailer)));
}

static void deliver_card(char marker) {
    iso14a_raw_resp_t header = {.len = sizeof(iso14a_card_select_t), .sel = 1};
    iso14a_card_select_t card = {.uidlen = 4, .uid = {(uint8_t)marker, 0x10, 0x20, 0x30}};
    uint8_t payload[sizeof(header) + sizeof(card)];
    memcpy(payload, &header, sizeof(header));
    memcpy(payload + sizeof(header), &card, sizeof(card));
    deliver(CMD_HF_ISO14443A_READER, payload, sizeof(payload));
}

uint32_t furi_get_tick(void) { return tick; }

void furi_delay_ms(uint32_t ms) {
    tick += ms;
    if(cancel_due && tick >= cancel_due) {
        cancel_due = 0;
        canceled = true;
    }
    if(stale_due && tick >= stale_due) {
        stale_due = 0;
        deliver(CMD_PING, stale_nonce, sizeof(stale_nonce));
    }
    if(old_due && tick >= old_due) {
        old_due = 0;
        deliver_card('A');
    }
    if(echo_due && tick >= echo_due) {
        echo_due = 0;
        deliver(CMD_PING, nonce, sizeof(nonce));
    }
    if(new_due && tick >= new_due) {
        new_due = 0;
        deliver_card('B');
    }
}

void proxmark5_com_set_waiting(bool waiting) {
    assert(waiting != waiting_for_reply);
    waiting_for_reply = waiting;
}

bool proxmark5_com_send_spi(uint8_t* data, size_t length) {
    assert(!waiting_for_reply);
    PacketCommandNGPreamble header;
    assert(length >= sizeof(header) + sizeof(PacketCommandNGPostamble));
    memcpy(&header, data, sizeof(header));
    if(header.cmd == CMD_HF_ISO14443A_READER) {
        if(!selects++) old_due = tick + 100;
        else new_due = tick + 80;
        return true;
    }
    assert(header.cmd == CMD_PING && header.length == sizeof(nonce));
    assert(memcmp(nonce, data + sizeof(header), sizeof(nonce)) != 0);
    if(delay_first_echo) memcpy(stale_nonce, nonce, sizeof(nonce));
    memcpy(nonce, data + sizeof(header), sizeof(nonce));
    pings++;
    if(delay_first_echo && pings == 1) return true;
    if(link_active) {
        stale_due = tick + 10;
        if(!suppress_echo) {
            echo_due = tick + 120;
        }
    }
    return true;
}

bool proxmark5_com_handshake(void) {
    handshakes++;
    if(handshake_ok) link_active = true;
    return handshake_ok;
}
'''

CASES = r'''
static int scan(iso14a_card_select_t* card, bool* found) {
    if(!proxmark5_frame_begin_operation(&canceled)) return PM3_EOPABORTED;
    int status = read_hf14a_test(&canceled, card, found);
    proxmark5_frame_end_operation();
    return status;
}

static void setup(void) {
    tick = old_due = new_due = echo_due = stale_due = cancel_due = 0;
    selects = pings = handshakes = 0;
    canceled = suppress_echo = handshake_ok = false;
    delay_first_echo = false;
    link_active = true;
    memset(nonce, 0, sizeof(nonce));
    memset(stale_nonce, 0, sizeof(stale_nonce));
    proxmark5_frame_init();
}

static void queue_canceled_scan(void) {
    iso14a_card_select_t card;
    bool found;
    cancel_due = 10;
    assert(scan(&card, &found) == PM3_EOPABORTED);
    assert(!found && old_due == 100 && selects == 1);
    canceled = false;
}

int main(void) {
    iso14a_card_select_t card;
    bool found;

    setup();
    queue_canceled_scan();
    assert(scan(&card, &found) == PM3_SUCCESS && found && card.uid[0] == 'B');
    assert(selects == 2 && pings == 1);
    assert(scan(&card, &found) == PM3_SUCCESS && card.uid[0] == 'B');
    assert(selects == 3 && pings == 1);
    assert(!waiting_for_reply);
    proxmark5_frame_deinit();

    setup();
    queue_canceled_scan();
    suppress_echo = true;
    assert(scan(&card, &found) == PM3_EIO && !found);
    assert(selects == 1 && pings == 3 && handshakes == 1);
    assert(!waiting_for_reply);
    proxmark5_frame_deinit();

    setup();
    queue_canceled_scan();
    old_due = 0;
    link_active = false;
    handshake_ok = true;
    assert(scan(&card, &found) == PM3_SUCCESS && found && card.uid[0] == 'B');
    assert(selects == 2 && pings == 4 && handshakes == 1);
    assert(!waiting_for_reply);
    proxmark5_frame_deinit();

    setup();
    queue_canceled_scan();
    cancel_due = tick + 20;
    assert(scan(&card, &found) == PM3_EOPABORTED && !found);
    assert(selects == 1 && pings == 1 && handshakes == 0);
    assert(!waiting_for_reply);
    proxmark5_frame_deinit();

    setup();
    delay_first_echo = true;
    uint32_t rtt_ms = 0;
    proxmark5_frame_begin_operation(NULL);
    assert(proxmark5_frame_echo(&rtt_ms));
    proxmark5_frame_end_operation();
    assert(pings == 2 && tick == 620 && rtt_ms == 120);
    assert(!waiting_for_reply);
    proxmark5_frame_deinit();

    setup();
    suppress_echo = true;
    proxmark5_frame_begin_operation(NULL);
    assert(!proxmark5_frame_echo(&rtt_ms));
    assert(SendCommandNG(CMD_HF_ISO14443A_READER, NULL, 0));
    assert(pings == 3 && selects == 1);
    proxmark5_frame_end_operation();
    proxmark5_frame_begin_operation(NULL);
    assert(!SendCommandNG(CMD_HF_ISO14443A_READER, NULL, 0));
    assert(pings == 6 && selects == 1 && handshakes == 1);
    proxmark5_frame_end_operation();
    assert(!waiting_for_reply);
    proxmark5_frame_deinit();
    setup();
    assert(proxmark5_frame_begin_operation(NULL));
    cancel_due = 20;
    assert(!proxmark5_frame_begin_operation(&canceled));
    assert(tick == 20);
    proxmark5_frame_end_operation();
    canceled = false;
    assert(proxmark5_frame_begin_operation(&canceled));
    cancel_due = tick + 10;
    PacketResponseNG pending;
    assert(!WaitForResponseTimeout(CMD_PING, &pending, 5000));
    assert(tick == 30 && !waiting_for_reply);
    proxmark5_frame_set_operation_cancel(NULL);
    assert(!proxmark5_frame_operation_cancelled());
    proxmark5_frame_end_operation();
    proxmark5_frame_deinit();
    puts("PM5 reply order: stale UID and echo discarded, restart recovered");
}
'''

source = (ROOT / "pages/read_hf14a_page.c").read_text()
reader = source[source.index("static int read_hf14a_test("):source.index("static void format_uid_hex(")]
with tempfile.TemporaryDirectory(prefix="pm5-reply-order-") as directory:
    temp = Path(directory)
    (temp / "furi_hal.h").write_text(HAL)
    (temp / "test.c").write_text(HOST + reader + CASES)
    executable = temp / "test"
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(temp), "-I", str(DRIVER),
        str(DRIVER / "proxmark5_frame.c"), str(temp / "test.c"), "-o", str(executable),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
