#!/usr/bin/env python3
"""Run the LF reader code with synthetic samples and PM5 replies."""

from pathlib import Path
import os
import subprocess
import tempfile

APP = Path(__file__).resolve().parent.parent

TEST = r'''
#include <assert.h>
#include <stdio.h>
#include "bigbuf.c"
#include "lf_capture.c"
#include "em4100.c"

static uint8_t samples[EM_SAMPLE_COUNT];
static unsigned received, range_size, range_received, downloads, capture_size;
static uint16_t peer_payload_size = PM3_CMD_DATA_SIZE;
static int bad_chunk;
static bool drop_first_download, drop_later_chunk, bad_first_download_only;
static unsigned cap_requests;
static unsigned missed_completion_replies;
static volatile bool canceled;
static const volatile bool* operation_cancel = &canceled;
static bool cancel_capture;
static unsigned range_fences;
static unsigned missed_cap_replies;
static bool block_ready;
static sample_config actual_config = {
    .decimation = 1, .bits_per_sample = 8, .divisor = 95,
};
static unsigned config_writes, em_requests;
static size_t max_heap_block = 8192;
static bool short_capture, fixture_em, config_needs_fence, lose_config_echo;

struct ProtocolDict { unsigned pulses; };
struct ProtocolBase { int unused; };
static const struct ProtocolBase em_protocol;
const ProtocolBase* const lfrfid_protocols[] = {&em_protocol, &em_protocol, &em_protocol};
ProtocolDict* protocol_dict_alloc(const ProtocolBase* const* modes, size_t count) {
    assert(modes && count == 3);
    return calloc(1, sizeof(ProtocolDict));
}
void protocol_dict_free(ProtocolDict* dict) { free(dict); }
void protocol_dict_decoders_start(ProtocolDict* dict) { dict->pulses = 0; }
ProtocolId protocol_dict_decoders_feed(ProtocolDict* dict, bool level, uint32_t duration) {
    (void)level;
    (void)duration;
    return fixture_em && ++dict->pulses % 16 == 0 ? 0 : PROTOCOL_NO;
}
void protocol_dict_get_data(ProtocolDict* dict, ProtocolId protocol, uint8_t* id, size_t length) {
    assert(dict && protocol == 0 && length == 5);
    memcpy(id, (uint8_t[]){1, 2, 3, 4, 5}, length);
}

static int32_t kernel_lock;
int32_t furi_kernel_lock(void) {
    assert(!kernel_lock);
    kernel_lock = 1;
    return 0;
}
int32_t furi_kernel_unlock(void) {
    assert(kernel_lock);
    kernel_lock = 0;
    return 1;
}
size_t memmgr_heap_get_max_free_block(void) {
    assert(kernel_lock);
    return max_heap_block;
}
void clearCommandBuffer(void) {
}
uint32_t furi_get_tick(void) {
    static uint32_t tick;
    return ++tick;
}
bool proxmark5_frame_operation_cancelled(void) {
    return operation_cancel && *operation_cancel;
}
const volatile bool* proxmark5_frame_set_operation_cancel(const volatile bool* cancel) {
    const volatile bool* previous = operation_cancel;
    operation_cancel = cancel;
    return previous;
}
bool proxmark5_frame_synchronize(void) {
    range_fences++;
    if(config_needs_fence && lose_config_echo) return false;
    config_needs_fence = false;
    return true;
}
bool SendCommandNG(uint16_t cmd, uint8_t* data, size_t length) {
    switch(cmd) {
    case CMD_CAPABILITIES:
        assert(length == 0 && block_ready);
        cap_requests++;
        break;
    case CMD_DOWNLOAD_BIGBUF:
        assert(length == 8 && read_le32(data) == received);
        range_size = read_le32(data + 4);
        range_received = 0;
        downloads++;
        assert(range_size && received + range_size <= capture_size);
        if(peer_payload_size <= PM3_CMD_DATA_SIZE) {
            assert(range_size == EM_SAMPLES_PER_BLOCK);
        } else {
            assert(range_size == MIN(PM3_CMD_DATA_SIZE - 4U, capture_size - received));
        }
        break;
    case CMD_LF_SAMPLING_GET_CONFIG:
        assert(length == 0 && !config_needs_fence);
        break;
    case CMD_LF_SAMPLING_SET_CONFIG:
        assert(length == sizeof(sample_config));
        assert(data[offsetof(sample_config, verbose)] == 0);
        memcpy(&actual_config, data, offsetof(sample_config, verbose));
        config_needs_fence = true;
        config_writes++;
        break;
    case CMD_LF_ACQ_RAW_ADC:
        assert(length == 5 && data[4] == 0 && read_le32(data) == EM_SAMPLE_COUNT);
        assert(actual_config.divisor == 95 && actual_config.decimation == 1 &&
               actual_config.bits_per_sample == 8 && actual_config.trigger_threshold == 0 &&
               actual_config.samples_to_skip == 0);
        capture_size = EM_SAMPLE_COUNT;
        received = downloads = 0;
        em_requests++;
        if(cancel_capture) canceled = true;
        break;
    default:
        assert(false);
    }
    return true;
}
bool WaitForResponseTimeout(uint16_t cmd, PacketResponseNG* p, uint32_t timeout) {
    (void)timeout;
    if(proxmark5_frame_operation_cancelled()) return false;
    memset(p, 0, sizeof(*p));
    if(cmd == CMD_CAPABILITIES) {
        if(missed_cap_replies) {
            missed_cap_replies--;
            return false;
        }
        capabilities_t caps = {.version = CAPABILITIES_VERSION, .max_cmd_data_size = peer_payload_size};
        p->length = sizeof(caps);
        memcpy(p->data.asBytes, &caps, sizeof(caps));
    }
    if(cmd == CMD_LF_SAMPLING_GET_CONFIG) {
        p->length = sizeof(actual_config);
        memcpy(p->data.asBytes, &actual_config, sizeof(actual_config));
    }
    if(cmd == CMD_LF_ACQ_RAW_ADC) {
        p->length = sizeof(uint32_t);
        p->data.asDwords[0] = 8U * capture_size - (short_capture ? 8U : 0U);
        block_ready = true;
    }
    if(cmd == CMD_DOWNLOADED_BIGBUF) {
        if((drop_first_download && downloads == 1) || (drop_later_chunk && range_received)) {
            return false;
        }
        unsigned count = MIN(peer_payload_size - 4U, range_size - range_received);
        assert(count && count <= PM3_CMD_DATA_SIZE - 4);
        assert(received + range_received + count <= capture_size);
        bool bad_offset = bad_chunk && (!bad_first_download_only || downloads == 1);
        uint32_t offset = range_received + bad_offset;
        memcpy(p->data.asBytes, &offset, sizeof(offset));
        p->length = count + 4;
        memcpy(p->data.asBytes + 4, samples + received + range_received, count);
        range_received += count;
    }
    if(cmd == CMD_DOWNLOAD_BIGBUF) {
        if(missed_completion_replies) {
            missed_completion_replies--;
            return false;
        }
        if(drop_later_chunk) return false;
        p->length = 8;
        uint32_t count = range_size;
        memcpy(p->data.asBytes, &count, sizeof(count));
        if(!bad_chunk || (bad_first_download_only && downloads > 1)) {
            assert(range_received == range_size);
            received += range_size;
        }
    }
    return true;
}

void saved_em4100_save(const uint8_t* id) { (void)id; }

int main(void) {
    uint8_t tag[5];
    SimpleReaderResult result = {.data = tag};
    fixture_em = true;
    for(size_t i = 0; i < sizeof(samples); i++) samples[i] = (i % 64 < 32) ? 150 : 90;
    assert(simple_reader_em4100.read(&result) == PM3_SUCCESS && result.line_count == 1);
    assert(strcmp(result.lines[0], "ID: 0102030405") == 0);
    assert(em_requests == 1 && downloads == EM_SAMPLE_BLOCKS && config_writes == 0);

    const sample_config before = {
        .decimation = 4, .bits_per_sample = 5, .averaging = 1, .divisor = 88,
        .trigger_threshold = 18, .samples_to_skip = 100, .verbose = true,
    };
    actual_config = before;
    assert(read_em4100_id(tag) == PM3_SUCCESS);
    assert(config_writes == 2 && memcmp(&actual_config, &before, sizeof(before)) == 0);
    short_capture = true;
    assert(read_em4100_id(tag) == PM3_EPARTIAL && downloads == 0);
    assert(config_writes == 4 && memcmp(&actual_config, &before, sizeof(before)) == 0);
    short_capture = fixture_em = false;
    memset(samples, 128, sizeof(samples));
    assert(read_em4100_id(tag) == PM3_ENODATA && downloads == EM_SAMPLE_BLOCKS);
    lose_config_echo = true;
    assert(read_em4100_id(tag) == LfCaptureRestoreFailed);
    assert(memcmp(&actual_config, &before, sizeof(before)) == 0);
    lose_config_echo = config_needs_fence = false;

    max_heap_block = EM_SAMPLES_PER_BLOCK + 7;
    unsigned before_requests = em_requests;
    assert(read_em4100_id(tag) == PM3_EMALLOC && em_requests == before_requests);

    max_heap_block++;
    uint8_t* block = proxmark5_try_alloc(EM_SAMPLES_PER_BLOCK);
    assert(block);
    free(block);

    max_heap_block = 8192;
    cancel_capture = true;
    unsigned before_writes = config_writes;
    assert(read_em4100_id(tag) == PM3_EOPABORTED);
    assert(config_writes == before_writes + 2);
    assert(memcmp(&actual_config, &before, sizeof(before)) == 0);
    assert(operation_cancel == &canceled && canceled && downloads == 0);
    canceled = false;
    cancel_capture = false;

    uint8_t transfer[EM_SAMPLES_PER_BLOCK];
    capture_size = sizeof(transfer);
    received = downloads = range_fences = 0;
    block_ready = true;
    missed_completion_replies = 1;
    assert(bigbuf_download(transfer, 0, sizeof(transfer)) == PM3_SUCCESS);
    assert(downloads == 2 && range_fences == 1);
    assert(memcmp(transfer, samples, sizeof(transfer)) == 0);

    peer_payload_size = 4064;
    received = downloads = 0;
    for(size_t i = 0; i < sizeof(samples); i++) samples[i] = i % 251;
    assert(bigbuf_download(transfer, 0, sizeof(transfer)) == PM3_SUCCESS);
    assert(downloads == (sizeof(transfer) + 507) / 508 && received == sizeof(transfer));
    assert(memcmp(transfer, samples, sizeof(transfer)) == 0);

    received = downloads = cap_requests = 0;
    missed_cap_replies = 2;
    assert(bigbuf_download(transfer, 0, sizeof(transfer)) == PM3_ETIMEOUT);
    assert(cap_requests == 2 && downloads == 0);

    puts("LF reader tests passed");
}
'''

with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp / "furi.h").write_text(
        "#include <stdlib.h>\n#include <string.h>\n#include <stdint.h>\n#include <stdbool.h>\n"
        "int32_t furi_kernel_lock(void);\nint32_t furi_kernel_unlock(void);\n"
        "size_t memmgr_heap_get_max_free_block(void);\nuint32_t furi_get_tick(void);\n#define MIN(a, b) ((a) < (b) ? (a) : (b))\n"
        "#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))\n"
    )
    protocol_api = tmp / "toolbox/protocols/protocol_dict.h"
    protocol_api.parent.mkdir(parents=True)
    protocol_api.write_text(
        "#pragma once\n"
        "typedef int ProtocolId;\n#define PROTOCOL_NO (-1)\n"
        "typedef struct ProtocolDict ProtocolDict;\ntypedef struct ProtocolBase ProtocolBase;\n"
        "ProtocolDict* protocol_dict_alloc(const ProtocolBase* const*, size_t);\n"
        "void protocol_dict_free(ProtocolDict*);\nvoid protocol_dict_decoders_start(ProtocolDict*);\n"
        "ProtocolId protocol_dict_decoders_feed(ProtocolDict*, bool, uint32_t);\n"
        "void protocol_dict_get_data(ProtocolDict*, ProtocolId, uint8_t*, size_t);\n"
    )
    rfid_api = tmp / "lfrfid/protocols/lfrfid_protocols.h"
    rfid_api.parent.mkdir(parents=True)
    rfid_api.write_text(
        '#include "toolbox/protocols/protocol_dict.h"\n'
        "enum {LFRFIDProtocolEM4100, LFRFIDProtocolEM410032, LFRFIDProtocolEM410016};\n"
        "extern const ProtocolBase* const lfrfid_protocols[];\n"
    )
    (tmp / "test.c").write_text(TEST)
    exe = tmp / "lf_readers"
    subprocess.run(
        [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(tmp), "-I", str(APP / "readers"), "-I", str(APP / "lib/proxmark5_driver"),
         str(tmp / "test.c"), "-o", str(exe)],
        check=True,
    )
    subprocess.run([exe], check=True)
