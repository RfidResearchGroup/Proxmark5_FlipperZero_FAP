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

"""Run the app's Hitag2 read against a simulated PM5 command handler."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

PREAMBLE = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm3_cmd.h"
#include "hitag.h"
#define FURI_LOG_I(...) ((void)0)
#define FURI_LOG_W(...) ((void)0)
static uint32_t tick;
static struct { bool sync_needed; } frame_context;
static unsigned sends, breaks, authentications, clears;
static bool waiting_for_reply, cancel_after_reply;
static bool queued, silent, crypto, override_response, cancel_on_delay, send_failed;
// A is the tag UID, X is another UID, - is a miss, T is a timeout.
static const char* samples;
static volatile bool cancel;
static uint8_t tag_password[4];
static PacketResponseNG response;
static lf_hitag_data_t request;
static const uint8_t tag_uid[] = {0x12, 0x34, 0x56, 0x11};
static uint32_t furi_get_tick(void) { return tick; }
static void furi_delay_ms(uint32_t ms) {
    tick += ms;
    if(cancel_on_delay) cancel = true;
}
static void clearCommandBuffer(void) { clears++; queued = false; }
static bool SendCommandNG(uint16_t cmd, uint8_t* data, size_t size) {
    if(cmd == CMD_BREAK_LOOP) {
        assert(data == NULL && size == 0);
        breaks++;
        return true;
    }
    assert(cmd == CMD_LF_HITAG_READER && size == sizeof(request));
    memcpy(&request, data, size);
    sends++;
    if(send_failed) return false;
    if(!override_response) {
        memset(&response, 0, sizeof(response));
        response.cmd = cmd;
        if(request.cmd == HT2F_UID_ONLY) {
            response.status = PM3_SUCCESS;
            response.length = HITAG_UID_SIZE;
            memcpy(response.data.asBytes, tag_uid, sizeof(tag_uid));
        } else {
            assert(request.cmd == HT2F_PASSWORD);
            authentications++;
            response.length = 48;
            bool correct = !crypto && memcmp(request.pwd, tag_password, sizeof(tag_password)) == 0;
            response.status = correct ? PM3_SUCCESS : PM3_EFAILED;
            memcpy(response.data.asBytes, correct ? tag_uid : (const uint8_t*)"\xDE\xAD\xBE\xEF", 4);
        }
    }
    queued = !silent;
    if(samples) {
        char sample = samples[sends - 1];
        assert(sample);
        if(sample == 'X') response.data.asBytes[0] ^= 0xff;
        if(sample == '-') response.status = PM3_EFAILED;
        if(sample == 'T') queued = false;
    }
    return true;
}
static void proxmark5_com_set_waiting(bool waiting) {
    assert(waiting != waiting_for_reply);
    waiting_for_reply = waiting;
}
static bool proxmark5_frame_take_by_cmd(uint16_t cmd, PacketResponseNG* out) {
    assert(waiting_for_reply);
    if(!queued || response.cmd != cmd) return false;
    *out = response;
    queued = false;
    if(cancel_after_reply) cancel = true;
    return true;
}
'''

TEST = r'''
static void setup(void) {
    tick = sends = breaks = authentications = clears = 0;
    queued = silent = crypto = override_response = cancel_on_delay = cancel = send_failed = false;
    samples = NULL;
    cancel_after_reply = false;
    frame_context.sync_needed = false;
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    memcpy(tag_password, "\x12\x34\x56\x78", 4);
}
static void check(int expected, bool valid) {
    uint32_t uid = 0xDEADBEEF;
    assert(read_hitag2_scan(&cancel, &uid) == expected);
    assert(!waiting_for_reply);
    assert(uid == (valid ? 0x12345611 : 0xDEADBEEF));
}
int main(void) {
    setup();
    check(PM3_SUCCESS, true);
    assert(request.cmd == HT2F_UID_ONLY && sends == 2 && authentications == 0);
    assert(tick == 0);
    for(size_t i = 0; i < sizeof(request.pwd); i++) assert(request.pwd[i] == 0);
    setup(); memcpy(tag_password, "MIKR", 4);
    check(PM3_SUCCESS, true);
    assert(authentications == 0);
    setup(); crypto = true;
    check(PM3_SUCCESS, true);
    assert(authentications == 0);

    setup(); samples = "--AA";
    check(PM3_SUCCESS, true);
    assert(sends == 4);
    setup(); samples = "A-A";
    check(PM3_SUCCESS, true);
    assert(sends == 3);
    setup(); samples = "AXX-";
    check(PM3_EWRONGANSWER, false);
    assert(sends == 2);
    setup(); samples = "-XAA";
    check(PM3_EWRONGANSWER, false);
    assert(sends == 3);
    setup(); samples = "-A--";
    check(PM3_EWRONGANSWER, false);
    assert(sends == 4);
    setup(); samples = "AT";
    check(PM3_ETIMEOUT, false);
    assert(sends == 2);
    setup(); samples = "AA"; cancel_after_reply = true;
    check(PM3_EOPABORTED, false);
    assert(sends == 1 && breaks == 0 && tick == 0);

    setup(); override_response = true;
    response.cmd = CMD_LF_HITAG_READER;
    response.status = PM3_EFAILED;
    response.length = HITAG_UID_SIZE;
    memcpy(response.data.asBytes, "\xDE\xAD\xBE\xEF", 4);
    check(PM3_EFAILED, false);
    assert(sends == 4);

    const uint16_t lengths[] = {0, 1, 3, 5, 48};
    for(size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        setup(); override_response = true;
        response.cmd = CMD_LF_HITAG_READER;
        response.status = PM3_SUCCESS;
        response.length = lengths[i];
        check(PM3_ESOFT, false);
    }
    setup(); silent = true;
    check(PM3_ETIMEOUT, false);
    assert(breaks == 0 && sends == 1 && tick == 2000);
    setup(); cancel = true;
    check(PM3_EOPABORTED, false);
    assert(sends == 0 && breaks == 0 && clears == 0);
    setup(); silent = true; cancel_on_delay = true;
    check(PM3_EOPABORTED, false);
    assert(sends == 1 && breaks == 0 && tick == 10);
    setup(); send_failed = true;
    check(PM3_EIO, false);
    assert(sends == 1 && tick == 0);
    setup(); override_response = true;
    response.cmd = CMD_LF_HITAG_READER;
    response.status = PM3_ENOTIMPL;
    check(PM3_ENOTIMPL, false);
    puts("Hitag2 UID tests: OK");
    return 0;
}
'''


def extract(source, name):
    match = re.search(r"(?:static )?(?:int|void)\s+" + name + r"\(", source)
    if not match:
        raise ValueError("Missing function: " + name)
    end = source.index("\n}", match.start()) + 2
    return source[match.start():end] + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, help="read page source for negative controls")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = (args.source or root / "pages/read_hitag2_page.c").read_text()
    frame_source = (root / "lib/proxmark5_driver/proxmark5_frame.c").read_text()
    constants = re.findall(r"^#define HITAG2_UID_ATTEMPTS \d+$", source, re.MULTILINE)
    functions = "\n".join(constants) + "\n" + extract(frame_source, "proxmark5_frame_wait") + "".join(
        extract(source, name)
        for name in (
            "read_hitag2_uid",
            "read_hitag2_scan",
        )
    )
    with tempfile.TemporaryDirectory(prefix="hitag2-uid-test-") as directory:
        executable = Path(directory) / "hitag2-uid-test"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-O2", "-Wall", "-Werror", "-I", str(root / "lib/proxmark5_driver"),
            "-x", "c", "-", "-o", str(executable),
        ]
        subprocess.run(command, input=PREAMBLE + functions + TEST, text=True, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
