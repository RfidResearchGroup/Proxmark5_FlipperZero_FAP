#include <furi_hal.h>
#include <inttypes.h>
#include <string.h>

#include "proxmark5_com.h"
#include "proxmark5_frame.h"

#define PROXMARK5_LOG_TAG "Proxmark5_FRAME"

#define PM3_RESPONSE_NG_MAGIC       0x62334D50UL // "PM3b"
#define PM3_RESPONSE_NG_POST_MAGIC  0x3362U // "b3"
#define PM3_RESPONSE_NG_HEADER_SIZE 10U
#define PM3_RESPONSE_NG_POST_SIZE   2U

#define PM3_CMD_DEBUG_PRINT_STRING   0x0100U
#define PM3_CMD_DEBUG_PRINT_INTEGERS 0x0101U

#define PM3_FLAG_LOG     0x01U
#define PM3_FLAG_NEWLINE 0x02U
#define PM3_FLAG_INPLACE 0x04U

#define PM3_RESPONSE_CACHE_SIZE      16U
#define PM3_OLD_RESPONSE_HEADER_SIZE (sizeof(uint64_t) * 4U)

#define CMD_UNKNOWN        0x0000U
#define PM3_SUCCESS        0
#define PM3_EOVFLOW        -17
#define PM3_REASON_UNKNOWN 0

typedef struct {
    uint64_t cmd;
    uint64_t arg[3];
    union {
        uint8_t asBytes[PM3_CMD_DATA_SIZE];
        uint32_t asDwords[PM3_CMD_DATA_SIZE / 4U];
    } d;
} PacketResponseOLD;

typedef struct {
    uint32_t magic;
    uint16_t length : 15;
    bool ng         : 1;
    int8_t status;
    int8_t reason;
    uint16_t cmd;
} PacketResponseNGPreamble;

typedef struct {
    uint16_t crc;
} PacketResponseNGPostamble;

typedef struct {
    PacketResponseNGPreamble pre;
    uint8_t data[PM3_CMD_DATA_SIZE];
    PacketResponseNGPostamble foopost;
} PacketResponseNGRaw;

typedef struct {
    PacketResponseNG cache[PM3_RESPONSE_CACHE_SIZE];
    uint8_t head;
    uint8_t tail;
} Proxmark5FrameContext;

static Proxmark5FrameContext frame_context;

static uint16_t pm5_u16_le(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t pm5_u32_le(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t pm5_u64_le(const uint8_t* data) {
    return (uint64_t)data[0] | ((uint64_t)data[1] << 8) | ((uint64_t)data[2] << 16) |
           ((uint64_t)data[3] << 24) | ((uint64_t)data[4] << 32) | ((uint64_t)data[5] << 40) |
           ((uint64_t)data[6] << 48) | ((uint64_t)data[7] << 56);
}

static uint16_t proxmark5_frame_crc14443a(uint8_t* data, size_t length) {
    uint16_t crc = 0x6363U;

    for(size_t i = 0; i < length; i++) {
        uint8_t ch = data[i] ^ (uint8_t)(crc & 0x00FFU);
        ch ^= (uint8_t)(ch << 4);
        crc = (crc >> 8) ^ ((uint16_t)ch << 8) ^ ((uint16_t)ch << 3) ^ ((uint16_t)ch >> 4);
    }

    return crc;
}

static bool proxmark5_frame_enqueue(PacketResponseNG* packet) {
    uint8_t next_head = (uint8_t)((frame_context.head + 1U) % PM3_RESPONSE_CACHE_SIZE);
    if(next_head == frame_context.tail) {
        FURI_LOG_W(PROXMARK5_LOG_TAG, "Packet cache overflow, dropping oldest entry");
        frame_context.tail = (uint8_t)((frame_context.tail + 1U) % PM3_RESPONSE_CACHE_SIZE);
    }

    frame_context.cache[frame_context.head] = *packet;
    frame_context.head = next_head;
    return true;
}

static int proxmark5_frame_reply_ng_internal(
    uint16_t cmd,
    int8_t status,
    uint8_t reason,
    uint8_t* data,
    size_t len,
    bool ng) {
    PacketResponseNGRaw tx_buffer;
    memset(&tx_buffer, 0, sizeof(tx_buffer));

    tx_buffer.pre.magic = PM3_RESPONSE_NG_MAGIC;
    tx_buffer.pre.cmd = cmd;
    tx_buffer.pre.status = status;
    tx_buffer.pre.reason = (int8_t)reason;
    tx_buffer.pre.ng = ng;
    if(len > PM3_CMD_DATA_SIZE) {
        len = PM3_CMD_DATA_SIZE;
        tx_buffer.pre.status = PM3_EOVFLOW;
    }

    tx_buffer.pre.length = (uint16_t)(len & 0x7FFFU);
    if(data != NULL && len != 0U) {
        memcpy(tx_buffer.data, data, len);
    }

    PacketResponseNGPostamble* tx_post =
        (PacketResponseNGPostamble*)((uint8_t*)&tx_buffer.pre + sizeof(PacketResponseNGPreamble) +
                                     len);
    tx_post->crc =
        proxmark5_frame_crc14443a((uint8_t*)&tx_buffer, sizeof(PacketResponseNGPreamble) + len);

    return proxmark5_com_send_spi(
        (uint8_t*)&tx_buffer,
        sizeof(PacketResponseNGPreamble) + len + sizeof(PacketResponseNGPostamble));
}

static void proxmark5_frame_log_print_string(uint8_t* data, size_t length, uint16_t flag) {
    char log_message[PM3_CMD_DATA_SIZE + 1U];
    size_t copy_len = length > PM3_CMD_DATA_SIZE ? PM3_CMD_DATA_SIZE : length;
    memcpy(log_message, data, copy_len);
    log_message[copy_len] = '\0';

    if(flag & PM3_FLAG_LOG) {
        FURI_LOG_I(PROXMARK5_LOG_TAG, "[#] %s", log_message);
    } else if(flag & PM3_FLAG_INPLACE) {
        FURI_LOG_I(PROXMARK5_LOG_TAG, "[inplace] %s", log_message);
    } else if(flag & PM3_FLAG_NEWLINE) {
        FURI_LOG_I(PROXMARK5_LOG_TAG, "%s", log_message);
    } else {
        FURI_LOG_I(PROXMARK5_LOG_TAG, "%s", log_message);
    }
}

static bool proxmark5_frame_handle_ng_packet(uint8_t* packet, size_t packet_len) {
    uint16_t length_and_ng = pm5_u16_le(packet + 4);
    bool ng = (length_and_ng & 0x8000U) != 0;
    uint16_t payload_length = length_and_ng & 0x7FFFU;
    int8_t status = (int8_t)packet[6];
    int8_t reason = (int8_t)packet[7];
    uint16_t cmd = pm5_u16_le(packet + 8);

    size_t expected_len =
        (size_t)PM3_RESPONSE_NG_HEADER_SIZE + payload_length + PM3_RESPONSE_NG_POST_SIZE;
    if(packet_len < expected_len) {
        return false;
    }

    uint16_t packet_crc = pm5_u16_le(packet + PM3_RESPONSE_NG_HEADER_SIZE + payload_length);
    if(packet_crc != PM3_RESPONSE_NG_POST_MAGIC) {
        uint16_t computed_crc =
            proxmark5_frame_crc14443a(packet, PM3_RESPONSE_NG_HEADER_SIZE + payload_length);
        if(packet_crc != computed_crc) {
            FURI_LOG_W(
                PROXMARK5_LOG_TAG,
                "Invalid NG response CRC: got 0x%04X expected 0x%04X",
                packet_crc,
                computed_crc);
            return false;
        }
    }

    uint8_t* payload = packet + PM3_RESPONSE_NG_HEADER_SIZE;
    if(cmd == PM3_CMD_DEBUG_PRINT_STRING) {
        if(ng) {
            if(payload_length < sizeof(uint16_t)) {
                return true;
            }
            uint16_t flag = pm5_u16_le(payload);
            proxmark5_frame_log_print_string(
                payload + sizeof(uint16_t), payload_length - sizeof(uint16_t), flag);
            return true;
        }

        if(payload_length < (sizeof(uint64_t) * 3U)) {
            return true;
        }

        uint64_t old_len64 = pm5_u64_le(payload);
        uint64_t old_flag64 = pm5_u64_le(payload + sizeof(uint64_t));
        size_t text_len = (size_t)old_len64;
        size_t max_text_len = payload_length - (sizeof(uint64_t) * 3U);
        if(text_len > max_text_len) {
            text_len = max_text_len;
        }

        proxmark5_frame_log_print_string(
            payload + (sizeof(uint64_t) * 3U), text_len, (uint16_t)old_flag64);
        return true;
    }

    if(cmd == PM3_CMD_DEBUG_PRINT_INTEGERS) {
        if(!ng && payload_length >= (sizeof(uint64_t) * 3U)) {
            uint64_t a0 = pm5_u64_le(payload);
            uint64_t a1 = pm5_u64_le(payload + sizeof(uint64_t));
            uint64_t a2 = pm5_u64_le(payload + (sizeof(uint64_t) * 2U));
            FURI_LOG_I(PROXMARK5_LOG_TAG, "[#] %" PRIx64 ", %" PRIx64 ", %" PRIx64, a0, a1, a2);
        } else {
            FURI_LOG_I(PROXMARK5_LOG_TAG, "[#] debug integers packet received");
        }
        return true;
    }

    PacketResponseNG response;
    memset(&response, 0, sizeof(response));
    response.magic = PM3_RESPONSE_NG_MAGIC;
    response.ng = ng;
    response.cmd = cmd;
    response.status = status;
    response.reason = reason;
    response.crc = packet_crc;

    if(ng) {
        size_t copy_len = payload_length > PM3_CMD_DATA_SIZE ? PM3_CMD_DATA_SIZE : payload_length;
        memcpy(response.data.asBytes, payload, copy_len);
        response.length = (uint16_t)copy_len;
    } else {
        if(payload_length >= (sizeof(uint64_t) * 3U)) {
            response.oldarg[0] = pm5_u64_le(payload);
            response.oldarg[1] = pm5_u64_le(payload + sizeof(uint64_t));
            response.oldarg[2] = pm5_u64_le(payload + (sizeof(uint64_t) * 2U));
            size_t data_len = payload_length - (sizeof(uint64_t) * 3U);
            size_t copy_len = data_len > PM3_CMD_DATA_SIZE ? PM3_CMD_DATA_SIZE : data_len;
            memcpy(response.data.asBytes, payload + (sizeof(uint64_t) * 3U), copy_len);
            response.length = (uint16_t)copy_len;
        }
    }

    return proxmark5_frame_enqueue(&response);
}

static bool proxmark5_frame_handle_old_packet(uint8_t* packet, size_t packet_len) {
    if(packet_len < PM3_OLD_RESPONSE_HEADER_SIZE) {
        return false;
    }

    uint16_t cmd = (uint16_t)pm5_u64_le(packet);
    if(cmd == PM3_CMD_DEBUG_PRINT_STRING) {
        uint64_t old_len64 = pm5_u64_le(packet + sizeof(uint64_t));
        uint64_t old_flag64 = pm5_u64_le(packet + (sizeof(uint64_t) * 2U));
        size_t text_len = (size_t)old_len64;
        size_t max_text_len = packet_len - PM3_OLD_RESPONSE_HEADER_SIZE;
        if(text_len > max_text_len) {
            text_len = max_text_len;
        }

        proxmark5_frame_log_print_string(
            packet + PM3_OLD_RESPONSE_HEADER_SIZE, text_len, (uint16_t)old_flag64);
        return true;
    }

    if(cmd == PM3_CMD_DEBUG_PRINT_INTEGERS) {
        uint64_t a0 = pm5_u64_le(packet + sizeof(uint64_t));
        uint64_t a1 = pm5_u64_le(packet + (sizeof(uint64_t) * 2U));
        uint64_t a2 = pm5_u64_le(packet + (sizeof(uint64_t) * 3U));
        FURI_LOG_I(PROXMARK5_LOG_TAG, "[#] %" PRIx64 ", %" PRIx64 ", %" PRIx64, a0, a1, a2);
        return true;
    }

    PacketResponseNG response;
    memset(&response, 0, sizeof(response));
    response.ng = false;
    response.cmd = cmd;
    response.oldarg[0] = pm5_u64_le(packet + sizeof(uint64_t));
    response.oldarg[1] = pm5_u64_le(packet + (sizeof(uint64_t) * 2U));
    response.oldarg[2] = pm5_u64_le(packet + (sizeof(uint64_t) * 3U));

    size_t data_len = packet_len - PM3_OLD_RESPONSE_HEADER_SIZE;
    size_t copy_len = data_len > PM3_CMD_DATA_SIZE ? PM3_CMD_DATA_SIZE : data_len;
    memcpy(response.data.asBytes, packet + PM3_OLD_RESPONSE_HEADER_SIZE, copy_len);
    response.length = (uint16_t)copy_len;

    return proxmark5_frame_enqueue(&response);
}

void proxmark5_frame_init(void) {
    memset(&frame_context, 0, sizeof(frame_context));
}

void proxmark5_frame_reset(void) {
    frame_context.head = 0;
    frame_context.tail = 0;
}

bool proxmark5_frame_handle_packet(uint8_t* packet, size_t packet_len) {
    if(packet == NULL || packet_len == 0) {
        return false;
    }

    if(packet_len >= PM3_RESPONSE_NG_HEADER_SIZE && pm5_u32_le(packet) == PM3_RESPONSE_NG_MAGIC) {
        return proxmark5_frame_handle_ng_packet(packet, packet_len);
    }

    return proxmark5_frame_handle_old_packet(packet, packet_len);
}

bool proxmark5_frame_pop(PacketResponseNG* packet) {
    if(packet == NULL || frame_context.head == frame_context.tail) {
        return false;
    }

    *packet = frame_context.cache[frame_context.tail];
    frame_context.tail = (uint8_t)((frame_context.tail + 1U) % PM3_RESPONSE_CACHE_SIZE);
    return true;
}

bool proxmark5_frame_get_response(PacketResponseNG* packet) {
    return proxmark5_frame_pop(packet);
}

bool proxmark5_frame_take_by_cmd(uint16_t cmd, PacketResponseNG* packet) {
    if(packet == NULL || frame_context.head == frame_context.tail) {
        return false;
    }

    PacketResponseNG temp[PM3_RESPONSE_CACHE_SIZE];
    uint8_t temp_count = 0;
    bool found = false;
    PacketResponseNG found_packet;

    while(frame_context.head != frame_context.tail) {
        PacketResponseNG current = frame_context.cache[frame_context.tail];
        frame_context.tail = (uint8_t)((frame_context.tail + 1U) % PM3_RESPONSE_CACHE_SIZE);

        if(!found && current.cmd == cmd) {
            found = true;
            found_packet = current;
        } else {
            temp[temp_count++] = current;
        }
    }

    frame_context.head = 0;
    frame_context.tail = 0;
    for(uint8_t i = 0; i < temp_count; i++) {
        frame_context.cache[frame_context.head] = temp[i];
        frame_context.head = (uint8_t)((frame_context.head + 1U) % PM3_RESPONSE_CACHE_SIZE);
    }

    if(found) {
        *packet = found_packet;
    }

    return found;
}

bool proxmark5_frame_wait_response(uint16_t cmd, PacketResponseNG* packet, uint32_t timeout_ms) {
    uint32_t start_time = furi_get_tick();

    while((furi_get_tick() - start_time) < timeout_ms) {
        if(proxmark5_frame_take_by_cmd(cmd, packet)) {
            return true;
        }
        furi_delay_ms(10);
    }

    return proxmark5_frame_take_by_cmd(cmd, packet);
}

int reply_old(uint64_t cmd, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* data, size_t len) {
    PacketResponseOLD txcmd;
    memset(&txcmd, 0, sizeof(txcmd));

    txcmd.cmd = cmd;
    txcmd.arg[0] = arg0;
    txcmd.arg[1] = arg1;
    txcmd.arg[2] = arg2;

    if(data != NULL && len != 0U) {
        size_t copy_len = len > PM3_CMD_DATA_SIZE ? PM3_CMD_DATA_SIZE : len;
        memcpy(txcmd.d.asBytes, data, copy_len);
    }

    return proxmark5_com_send_spi((uint8_t*)&txcmd, sizeof(txcmd));
}

int reply_ng(uint16_t cmd, int8_t status, uint8_t* data, size_t len) {
    return proxmark5_frame_reply_ng_internal(cmd, status, PM3_REASON_UNKNOWN, data, len, true);
}

int reply_mix(uint64_t cmd, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* data, size_t len) {
    int8_t status = PM3_SUCCESS;
    uint64_t arg[3] = {arg0, arg1, arg2};
    uint8_t cmddata[PM3_CMD_DATA_SIZE];
    size_t payload_len = len;

    memset(cmddata, 0, sizeof(cmddata));
    if(payload_len > PM3_CMD_DATA_SIZE - sizeof(arg)) {
        payload_len = PM3_CMD_DATA_SIZE - sizeof(arg);
        status = PM3_EOVFLOW;
    }

    memcpy(cmddata, arg, sizeof(arg));
    if(data != NULL && payload_len != 0U) {
        memcpy(cmddata + sizeof(arg), data, payload_len);
    }

    return proxmark5_frame_reply_ng_internal(
        (uint16_t)(cmd & 0xFFFFU),
        status,
        PM3_REASON_UNKNOWN,
        cmddata,
        payload_len + sizeof(arg),
        false);
}

int reply_reason(uint16_t cmd, int8_t status, int8_t reason, uint8_t* data, size_t len) {
    return proxmark5_frame_reply_ng_internal(cmd, status, (uint8_t)reason, data, len, true);
}
