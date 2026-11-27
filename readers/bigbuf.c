#include <furi.h>
#include "proxmark5_frame.h"
#include "bigbuf.h"

#define BIGBUF_REPLY_TIMEOUT_MS 1500
#define BIGBUF_FIRST_REPLY_MS 350
#define BIGBUF_CHUNK_HEADER_BYTES 4U

static uint32_t read_le32(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int bigbuf_reply_limit(uint16_t* limit) {
    PacketResponseNG reply;
    capabilities_t caps;
    for(unsigned attempt = 0; attempt < 2; attempt++) {
        clearCommandBuffer();
        if(!SendCommandNG(CMD_CAPABILITIES, NULL, 0)) return PM3_EIO;
        if(!WaitForResponseTimeout(CMD_CAPABILITIES, &reply, BIGBUF_FIRST_REPLY_MS)) continue;
        if(reply.status != PM3_SUCCESS) return reply.status;
        if(reply.length != sizeof(caps)) return PM3_ESOFT;

        memcpy(&caps, reply.data.asBytes, sizeof(caps));
        if(caps.version != CAPABILITIES_VERSION ||
           caps.max_cmd_data_size <= BIGBUF_CHUNK_HEADER_BYTES) {
            return PM3_ESOFT;
        }
        *limit = caps.max_cmd_data_size;
        return PM3_SUCCESS;
    }
    return PM3_ETIMEOUT;
}

static int bigbuf_receive_range(
    uint8_t* data,
    uint32_t count,
    size_t chunk_size,
    uint32_t first_timeout,
    bool* can_retry) {
    PacketResponseNG reply;
    int status = PM3_SUCCESS;
    for(size_t offset = 0; offset < count;) {
        uint32_t timeout = offset ? BIGBUF_REPLY_TIMEOUT_MS : first_timeout;
        if(!WaitForResponseTimeout(CMD_DOWNLOADED_BIGBUF, &reply, timeout)) {
            // With no data received, the command may have been missed entirely.
            if(!offset) {
                *can_retry = true;
                return PM3_ETIMEOUT;
            }
            status = PM3_ETIMEOUT;
            break;
        }
        size_t size = MIN(chunk_size, count - offset);
        if(reply.status != PM3_SUCCESS) {
            status = reply.status;
            break;
        }
        if(reply.length != size + BIGBUF_CHUNK_HEADER_BYTES ||
           read_le32(reply.data.asBytes) != offset) {
            status = PM3_ESOFT;
            break;
        }
        memcpy(data + offset, reply.data.asBytes + BIGBUF_CHUNK_HEADER_BYTES, size);
        offset += size;
    }

    // A dropped chunk may leave the rest of the transfer in flight.
    if(!WaitForResponseTimeout(CMD_DOWNLOAD_BIGBUF, &reply, BIGBUF_REPLY_TIMEOUT_MS)) {
        *can_retry = status == PM3_SUCCESS || status == PM3_ESOFT || status == PM3_ETIMEOUT;
        return PM3_ETIMEOUT;
    }
    if(reply.status != PM3_SUCCESS) return reply.status;
    if(reply.length != 8 || read_le32(reply.data.asBytes) != count) return PM3_ESOFT;
    *can_retry = status == PM3_ESOFT || status == PM3_ETIMEOUT;
    return status;
}

static int bigbuf_download_range(
    uint8_t* data,
    uint32_t start,
    uint32_t count,
    size_t chunk_size) {
    uint32_t request[] = {start, count};
    int status = PM3_ETIMEOUT;
    for(unsigned attempt = 0; attempt < 3; attempt++) {
        // BigBuf replies carry offsets relative to the request.
        if(attempt && !proxmark5_frame_synchronize()) {
            return proxmark5_frame_operation_cancelled() ? PM3_EOPABORTED : PM3_ETIMEOUT;
        }
        clearCommandBuffer();
        if(!SendCommandNG(CMD_DOWNLOAD_BIGBUF, (uint8_t*)request, sizeof(request))) {
            return PM3_EIO;
        }
        bool can_retry = false;
        uint32_t timeout = attempt < 2 ? BIGBUF_FIRST_REPLY_MS : BIGBUF_REPLY_TIMEOUT_MS;
        status = bigbuf_receive_range(data, count, chunk_size, timeout, &can_retry);
        if(proxmark5_frame_operation_cancelled()) return PM3_EOPABORTED;
        if(!can_retry) break;
    }
    return status;
}

int bigbuf_download(uint8_t* data, uint32_t start, uint32_t count) {
    if(!data || !count || count > UINT32_MAX - start) return PM3_EINVARG;

    uint16_t limit;
    int status = bigbuf_reply_limit(&limit);
    if(proxmark5_frame_operation_cancelled()) return PM3_EOPABORTED;
    if(status != PM3_SUCCESS) return status;
    if(limit <= PM3_CMD_DATA_SIZE) {
        return bigbuf_download_range(data, start, count, limit - BIGBUF_CHUNK_HEADER_BYTES);
    }

    // Bound each request so the PM5 cannot send a reply larger than the FAP buffer.
    const uint16_t chunk_size = PM3_CMD_DATA_SIZE - BIGBUF_CHUNK_HEADER_BYTES;
    for(uint32_t offset = 0; offset < count;) {
        uint32_t size = MIN(chunk_size, count - offset);
        status = bigbuf_download_range(data + offset, start + offset, size, chunk_size);
        if(status != PM3_SUCCESS) return status;
        offset += size;
    }
    return PM3_SUCCESS;
}
