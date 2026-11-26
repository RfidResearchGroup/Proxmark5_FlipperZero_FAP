#include <furi.h>
#include "bigbuf.h"
#include "hf14a_trace.h"
#include "proxmark5_frame.h"

#define TRACE_HEADER_SIZE 8U
#define TRACE_PROBE_SIZE 512U

static uint16_t read_le16(const uint8_t* p) {
    return p[0] | ((uint16_t)p[1] << 8);
}

bool hf14a_trace_frame(const uint8_t* data, size_t length, size_t offset, Hf14aTraceFrame* frame) {
    if(offset > length || length - offset < TRACE_HEADER_SIZE) return false;
    const uint8_t* header = data + offset;
    uint16_t info = read_le16(header + 6);
    size_t bytes = info & 0x7fff;
    size_t next = offset + TRACE_HEADER_SIZE + bytes + (bytes + 7) / 8;
    if(!bytes || next > length) return false;
    *frame = (Hf14aTraceFrame){
        .timestamp = read_le16(header) | ((uint32_t)read_le16(header + 2) << 16),
        .duration = read_le16(header + 4),
        .from_tag = (info & 0x8000) != 0,
        .length = bytes,
        .data = header + TRACE_HEADER_SIZE,
        .next = next,
    };
    return true;
}

int hf14a_trace_capture(uint8_t* data, uint32_t* length) {
    iso14a_raw_cmd_t select = {.flags = ISO14A_CONNECT | ISO14A_CLEARTRACE};
    PacketResponseNG reply;
    *length = 0;
    clearCommandBuffer();
    if(!SendCommandNG(CMD_HF_ISO14443A_READER, (uint8_t*)&select, sizeof(select))) return PM3_EIO;
    if(!WaitForResponseTimeout(CMD_HF_ISO14443A_READER, &reply, 2500)) return PM3_ETIMEOUT;
    if(reply.status != PM3_SUCCESS) return reply.status;
    if(reply.length != sizeof(iso14a_raw_resp_t) + sizeof(iso14a_card_select_t)) return PM3_ESOFT;

    uint32_t count;
    int status = bigbuf_download_with_trace_length(data, 0, TRACE_PROBE_SIZE, &count);
    if(status != PM3_SUCCESS) return status;
    if(!count) return PM3_ENODATA;
    if(count > HF14A_TRACE_CAPACITY) return PM3_EOVFLOW;
    if(count > TRACE_PROBE_SIZE) {
        uint32_t actual;
        status = bigbuf_download_with_trace_length(data, 0, count, &actual);
        if(status != PM3_SUCCESS) return status;
        if(actual != count) return PM3_ESOFT;
    }
    *length = count;
    return PM3_SUCCESS;
}
