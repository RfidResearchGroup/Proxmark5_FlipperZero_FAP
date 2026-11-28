#include <furi.h>
#include "../pages/saved_em4100.h"
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>
#include "bigbuf.h"
#include "lf_capture.h"
#include "proxmark5_alloc.h"
#include "proxmark5_frame.h"
#include "simple_readers.h"

// Four RF/64 frames plus one bit for the final edge.
#define EM_SAMPLE_COUNT ((4U * 64U + 1U) * 64U)
#define EM_SAMPLE_BLOCKS 4U
#define EM_SAMPLES_PER_BLOCK (EM_SAMPLE_COUNT / EM_SAMPLE_BLOCKS)

static uint8_t sample_at(uint8_t* const* samples, size_t index) {
    return samples[index / EM_SAMPLES_PER_BLOCK][index % EM_SAMPLES_PER_BLOCK];
}

static int percentile(const uint16_t histogram[256], size_t rank) {
    size_t sum = 0;
    for(int v = 0; v < 256; v++) {
        sum += histogram[v];
        if(sum > rank) return v;
    }
    return 255;
}

static int decode_pulses(
    ProtocolDict* dict,
    uint8_t* const* samples,
    size_t count,
    unsigned lower,
    unsigned upper,
    bool invert,
    uint8_t id[EM4100_ID_BYTES],
    bool* found) {
    protocol_dict_decoders_start(dict);
    bool level = samples[0][0] >= upper;
    size_t last_edge = 0;
    uint8_t candidate[EM4100_ID_BYTES];
    bool have_candidate = false;

    for(size_t i = 1; i < count; i++) {
        uint8_t value = sample_at(samples, i);
        if(level ? value > lower : value < upper) continue;
        const ProtocolId protocol =
            protocol_dict_decoders_feed(dict, level ^ invert, (i - last_edge) * 8U);
        level = !level;
        last_edge = i;
        if(protocol == PROTOCOL_NO) continue;

        uint8_t current[EM4100_ID_BYTES];
        protocol_dict_get_data(dict, protocol, current, sizeof(current));
        if(!have_candidate || memcmp(current, candidate, sizeof(current)) != 0) {
            memcpy(candidate, current, sizeof(current));
            have_candidate = true;
            continue;
        }
        if(*found && memcmp(id, current, sizeof(current)) != 0) return PM3_EWRONGANSWER;
        memcpy(id, current, sizeof(current));
        *found = true;
    }
    return PM3_SUCCESS;
}

static int decode_em(uint8_t* const* samples, size_t count, uint8_t id[EM4100_ID_BYTES]) {
    uint16_t histogram[256] = {0};
    for(size_t i = 0; i < count; i++) histogram[sample_at(samples, i)]++;
    const int bottom = percentile(histogram, count / 10);
    const int span = percentile(histogram, count * 9 / 10) - bottom;
    if(span < 8) return PM3_ENODATA;

    const ProtocolBase* modes[] = {
        lfrfid_protocols[LFRFIDProtocolEM4100],
        lfrfid_protocols[LFRFIDProtocolEM410032],
        lfrfid_protocols[LFRFIDProtocolEM410016],
    };
    bool found = false;
    for(int step = 1; step <= 5; step++) {
        const int center = bottom + span * step / 6;
        for(int divider = 3; divider <= 4; divider++) {
            const int lower = center - span / divider;
            const int upper = center + span / divider;
            if(lower < 0 || upper > 255) continue;
            for(unsigned invert = 0; invert < 2; invert++) {
                if(proxmark5_frame_operation_cancelled()) return PM3_EOPABORTED;
                // Reset the frame and epilogue between threshold and polarity passes.
                ProtocolDict* dict = protocol_dict_alloc(modes, COUNT_OF(modes));
                if(!dict) return PM3_EMALLOC;
                const int status = decode_pulses(dict, samples, count, lower, upper, invert, id, &found);
                protocol_dict_free(dict);
                if(status != PM3_SUCCESS) return status;
            }
        }
    }
    return found ? PM3_SUCCESS : PM3_ENODATA;
}

static int read_em4100_id(uint8_t id[EM4100_ID_BYTES]) {
    // A full capture can exceed the largest free heap block.
    uint8_t* samples[EM_SAMPLE_BLOCKS] = {0};
    int status = PM3_EMALLOC;
    for(size_t i = 0; i < COUNT_OF(samples); i++) {
        samples[i] = proxmark5_try_alloc(EM_SAMPLES_PER_BLOCK);
        if(!samples[i]) goto done;
    }

    status = lf_capture_samples(EM_SAMPLE_COUNT);
    for(size_t i = 0; status == PM3_SUCCESS && i < COUNT_OF(samples); i++) {
        status = bigbuf_download(samples[i], i * EM_SAMPLES_PER_BLOCK, EM_SAMPLES_PER_BLOCK);
    }
    if(status == PM3_SUCCESS) status = decode_em(samples, EM_SAMPLE_COUNT, id);

done:
    for(size_t i = 0; i < COUNT_OF(samples); i++) free(samples[i]);
    return status;
}

static int em4100_read(SimpleReaderResult* result) {
    uint8_t id[EM4100_ID_BYTES];
    int status = read_em4100_id(id);
    if(status == PM3_SUCCESS) {
        snprintf(
            result->lines[0], sizeof(result->lines[0]), "ID: %02X%02X%02X%02X%02X",
            id[0], id[1], id[2], id[3], id[4]);
        result->line_count = 1;
        memcpy(result->data, id, sizeof(id));
    }
    return status;
}

const SimpleReader simple_reader_em4100 = {
    .title = "EM4100 Reader",
    .prompt = "Hold tag to PM5 LF coil",
    .no_data = "No EM4100 ID read",
    .no_data_hint = "Move tag closer",
    .data_size = EM4100_ID_BYTES,
    .read = em4100_read,
    .save = saved_em4100_save,
};
