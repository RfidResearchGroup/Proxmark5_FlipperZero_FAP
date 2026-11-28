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

"""Check HF reader recovery and rapid reopen without Flipper hardware."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pm3_cmd.h"

typedef enum {
    ReadHf14aPageStateIdle, ReadHf14aPageStateRunning,
    ReadHf14aPageStateReconnecting, ReadHf14aPageStateDisconnected,
    ReadHf14aPageStateSuccess, ReadHf14aPageStateNotFound,
    ReadHf14aPageStateTimeout, ReadHf14aPageStateError,
} ReadHf14aPageState;
typedef struct {
    ReadHf14aPageState state;
    iso14a_card_select_t card;
    bool show_ats;
    uint8_t ats_page;
} ReadHf14aPageModel;
typedef enum {FuriThreadStateStopped, FuriThreadStateRunning} FuriThread;
typedef struct {
    ReadHf14aPageModel* view;
    FuriThread* worker_thread;
    bool worker_restart_requested;
    volatile bool worker_thread_cancel_requested;
} ReadHf14aPage;
void read_hf14a_page_start(ReadHf14aPage* page);
void read_hf14a_page_stop(ReadHf14aPage* page);
static ReadHf14aPage page;
static ReadHf14aPageModel model;
static FuriThread thread;
static int results[2], reads, handshakes, reconnect_screens, failures_remaining;
static bool handshake_ok, card_found, cancel_handshake;
static bool owns_link, model_locked;
static unsigned reopen;
static void* view_get_model(void* view) {
    assert(!model_locked);
    model_locked = true;
    return view;
}
static void view_commit_model(void* view, bool redraw) {
    assert(model_locked);
    model_locked = false;
    if(redraw && ((ReadHf14aPageModel*)view)->state == ReadHf14aPageStateReconnecting) {
        reconnect_screens++;
    }
}
static FuriThread furi_thread_get_state(FuriThread* worker) { return *worker; }
static void furi_thread_free(FuriThread* worker) {
    assert(*worker == FuriThreadStateStopped);
}
static void furi_thread_start(FuriThread* worker) {
    assert(*worker == FuriThreadStateStopped);
    *worker = FuriThreadStateRunning;
}
static FuriThread* furi_thread_alloc_ex(
    const char* name, unsigned size, int32_t (*callback)(void*), void* context) {
    (void)name; (void)size; (void)callback; (void)context;
    assert(thread == FuriThreadStateStopped);
    return &thread;
}
static int read_hf14a_test(volatile bool* cancel, iso14a_card_select_t* card, bool* found) {
    assert(owns_link && !*cancel && reads < 2);
    if(reopen && reads == 0) {
        read_hf14a_page_stop(&page);
        read_hf14a_page_start(&page);
        if(reopen == 2) read_hf14a_page_stop(&page);
    }
    memset(card, 0x42, sizeof(*card)); *found = card_found;
    return results[reads++];
}
static bool proxmark5_frame_begin_operation(const volatile bool* cancel) {
    assert(!owns_link && cancel == &page.worker_thread_cancel_requested);
    if(*cancel) return false;
    owns_link = true;
    return true;
}
static void proxmark5_frame_end_operation(void) {
    assert(owns_link);
    owns_link = false;
}
static bool proxmark5_frame_synchronize(void) { return owns_link; }
bool proxmark5_com_handshake(void) {
    handshakes++;
    if(failures_remaining > 0) { failures_remaining--; return false; }
    if(cancel_handshake) page.worker_thread_cancel_requested = true;
    return handshake_ok;
}
'''

CASES = r'''
static void check(int first, int second, bool connected, bool found, bool cancel,
                  ReadHf14aPageState expected, int expected_reads, int expected_handshakes) {
    memset(&model, 0, sizeof(model));
    page = (ReadHf14aPage){
        .view = &model, .worker_thread = &thread,
    };
    results[0] = first; results[1] = second;
    reads = handshakes = reconnect_screens = 0;
    handshake_ok = connected; card_found = found; cancel_handshake = cancel;
    thread = FuriThreadStateRunning;
    assert(read_hf14a_page_worker(&page) == 0);
    read_hf14a_page_tick(&page);
    assert(thread == FuriThreadStateRunning);
    thread = FuriThreadStateStopped;
    read_hf14a_page_tick(&page);
    if(thread == FuriThreadStateRunning) {
        assert(reopen == 1);
        assert(read_hf14a_page_worker(&page) == 0);
        thread = FuriThreadStateStopped;
    }
    if(reopen == 1) fprintf(stderr, "Quick reopen ran %d scan(s), expected %d\n", reads, expected_reads);
    assert(model.state == expected && thread == FuriThreadStateStopped && !owns_link);
    assert(!page.worker_restart_requested && !model_locked);
    assert(reads == expected_reads && handshakes == expected_handshakes);
    assert(reconnect_screens == (expected_handshakes > 0 ? 1 : 0));
    if(expected == ReadHf14aPageStateSuccess) assert(model.card.uid[0] == 0x42);
}
int main(void) {
    check(PM3_SUCCESS, 0, false, false, false, ReadHf14aPageStateNotFound, 1, 0);
    check(PM3_SUCCESS, 0, false, true, false, ReadHf14aPageStateSuccess, 1, 0);
    check(PM3_ETIMEOUT, PM3_SUCCESS, true, false, false, ReadHf14aPageStateNotFound, 2, 1);
    check(PM3_ETIMEOUT, PM3_SUCCESS, true, true, false, ReadHf14aPageStateSuccess, 2, 1);
    check(PM3_ETIMEOUT, 0, false, false, false, ReadHf14aPageStateDisconnected, 1, 3);
    check(PM3_ETIMEOUT, 0, true, false, true, ReadHf14aPageStateIdle, 1, 1);
    check(PM3_ESOFT, 0, true, false, false, ReadHf14aPageStateError, 1, 0);
    check(PM3_EOPABORTED, 0, true, false, false, ReadHf14aPageStateIdle, 1, 0);
    check(PM3_ETIMEOUT, PM3_ETIMEOUT, true, false, false, ReadHf14aPageStateTimeout, 2, 1);
    failures_remaining = 1;
    check(PM3_ETIMEOUT, PM3_SUCCESS, true, false, false, ReadHf14aPageStateNotFound, 2, 2);
    reopen = 1;
    check(PM3_EOPABORTED, PM3_SUCCESS, true, true, false, ReadHf14aPageStateSuccess, 2, 0);
    reopen = 2;
    check(PM3_EOPABORTED, 0, true, false, false, ReadHf14aPageStateIdle, 1, 0);
    puts("HF reader recovery and reopen: 12 cases passed");
}
'''

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path)
args = parser.parse_args()
source = (args.source or ROOT / "pages/read_hf14a_page.c").read_text()
worker = "static void read_hf14a_page_scan("
start = source.index(worker)
end = source.index("\nReadHf14aPage*", start)
controls = source[source.index("void read_hf14a_page_tick("):]
with tempfile.TemporaryDirectory(prefix="hf14a-reconnect-") as folder:
    source_file = Path(folder) / "test.c"
    executable = Path(folder) / "test"
    source_file.write_text(
        STUBS + "\n#define HF14A_RECONNECT_ATTEMPTS 3\n" + source[start:end] + controls + CASES
    )
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "lib/proxmark5_driver"),
        str(source_file), "-o", str(executable),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
