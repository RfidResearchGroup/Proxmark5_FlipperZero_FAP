#include <furi.h>
#include "bigbuf.h"
#include "../pages/saved_tag_read.h"
#include "lf_capture.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "simple_readers.h"

#define LF_REPLY_TIMEOUT_MS 1500
#define T5577_PING_TIMEOUT_MS 500
#define T5577_SAMPLE_COUNT 6144
#define T5577_CLOCK 32
#define T5577_CONFIG_SIZE 4
#define T5577_CONFIG_FIELDS 0x00088000U
#define T5577_SEQUENCE_TERMINATOR 0x08U
#define T5577_CONFIG_OPTIONAL (0x600000E0U | T5577_SEQUENCE_TERMINATOR)

static int t5577_capture_block(void) {
    PacketResponseNG reply;
    // CMD_LF_T55XX_READBL: password, block, page, password mode, downlink.
    uint8_t read[8] = {0};
    clearCommandBuffer();
    if(!SendCommandNG(CMD_LF_T55XX_READBL, read, sizeof(read))) return PM3_EIO;
    if(!WaitForResponseTimeout(CMD_LF_T55XX_READBL, &reply, LF_REPLY_TIMEOUT_MS)) {
        return PM3_ETIMEOUT;
    }
    return reply.status;
}

static int t5577_capture_config(uint8_t* samples) {
    int status = t5577_capture_block();
    if(status != PM3_SUCCESS) return status;
    return bigbuf_download(samples, 0, T5577_SAMPLE_COUNT);
}

static bool t5577_reconnect(void) {
    if(!proxmark5_com_handshake()) return false;
    PacketResponseNG reply;
    for(unsigned attempt = 0; attempt < 2; attempt++) {
        uint32_t token = furi_get_tick();
        clearCommandBuffer();
        if(!SendCommandNG(CMD_PING, (uint8_t*)&token, sizeof(token))) return false;
        if(WaitForResponseTimeout(CMD_PING, &reply, T5577_PING_TIMEOUT_MS) &&
           reply.status == PM3_SUCCESS && reply.length == sizeof(token) &&
           memcmp(reply.data.asBytes, &token, sizeof(token)) == 0) {
            return true;
        }
    }
    return false;
}

static bool t5577_manchester_config(const uint8_t* samples, size_t size, uint32_t* config) {
    if(size < 97 * T5577_CLOCK) return false;
    unsigned low = 255, high = 0;
    for(size_t i = 0; i < size; i++) {
        if(samples[i] < low) low = samples[i];
        if(samples[i] > high) high = samples[i];
    }
    if(high - low < 16) return false;
    unsigned threshold = (high + low) / 2;

    bool found = false;
    uint32_t matched = 0;
    for(size_t phase = 0; phase < T5577_CLOCK; phase++) {
        uint32_t word = 0, repeated = 0;
        size_t valid_bits = 0;
        size_t last_match = 0;
        unsigned repeats = 0;
        for(size_t i = phase; i + 24 < size; i += T5577_CLOCK) {
            bool first = samples[i + 8] > threshold;
            bool second = samples[i + 24] > threshold;
            if(first == second) {
                valid_bits = 0;
                continue;
            }
            word = (word << 1) | second;
            if(++valid_bits < 32) continue;

            // Only accept a unique, basic RF/32 Manchester configuration.
            uint32_t candidate = word;
            if((~candidate & ~T5577_CONFIG_OPTIONAL) == T5577_CONFIG_FIELDS) candidate = ~candidate;
            if((candidate & ~T5577_CONFIG_OPTIONAL) != T5577_CONFIG_FIELDS) continue;
            if((candidate >> 28) != 0 && (candidate >> 28) != 6) continue;
            // Ignore overlapping matches. A terminator interrupts the Manchester bit stream.
            size_t gap = i - last_match;
            size_t word_samples = 32 * T5577_CLOCK;
            if(repeats && gap < word_samples) continue;
            if(candidate != repeated || (gap != word_samples && !(candidate & T5577_SEQUENCE_TERMINATOR))) {
                repeats = 0;
            }
            repeated = candidate;
            last_match = i;
            if(++repeats < 3) continue;
            if(found && matched != candidate) return false;
            matched = candidate;
            found = true;
        }
    }
    if(found) *config = matched;
    return found;
}

static int read_t5577_config(uint8_t config[T5577_CONFIG_SIZE]) {
    uint8_t* samples = lf_capture_alloc(T5577_SAMPLE_COUNT);
    if(!samples) return PM3_EMALLOC;
    int status = t5577_capture_config(samples);
    if(status == PM3_ETIMEOUT && !proxmark5_frame_operation_cancelled() &&
       t5577_reconnect()) {
        status = t5577_capture_config(samples);
    }
    if(status == PM3_SUCCESS) {
        uint32_t word;
        if(t5577_manchester_config(samples, T5577_SAMPLE_COUNT, &word)) {
            for(size_t i = 0; i < 4; i++) config[i] = (uint8_t)(word >> (24 - i * 8));
        } else {
            status = PM3_ENODATA;
        }
    }
    free(samples);
    return status;
}

static int t5577_read(SimpleReaderResult* result) {
    uint8_t config[T5577_CONFIG_SIZE];
    int status = read_t5577_config(config);
    if(status == PM3_SUCCESS) {
        memcpy(result->data, config, sizeof(config));
        snprintf(
            result->lines[0], sizeof(result->lines[0]), "Config: %02X%02X%02X%02X",
            config[0], config[1], config[2], config[3]);
        snprintf(result->lines[1], sizeof(result->lines[1]), "RF/32 Manchester");
        snprintf(result->lines[2], sizeof(result->lines[2]), "Max block: %u", (config[3] >> 5) & 7U);
        result->line_count = 3;
    }
    return status;
}

static void t5577_save(const uint8_t* config) {
    SavedTagRead tag = {.type = SavedTagT5577};
    memcpy(tag.t5577_config, config, sizeof(tag.t5577_config));
    saved_tag_read_save(&tag);
}

const SimpleReader simple_reader_t5577 = {
    .title = "T5577 Info",
    .prompt = "Hold tag to PM5 LF coil",
    .no_data = "No supported config read",
    .no_data_hint = "Basic RF/32 Manchester",
    .data_size = T5577_CONFIG_SIZE,
    .read = t5577_read,
    .save = t5577_save,
};
