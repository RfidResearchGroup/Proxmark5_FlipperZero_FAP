#include <furi.h>
#include "proxmark5_frame.h"
#include "lf_capture.h"

#define LF_CARRIER_DIVISOR 95
#define LF_CONFIG_TIMEOUT_MS 1000
#define LF_CAPTURE_TIMEOUT_MS 3000

static int get_sampling_config(sample_config* config) {
    PacketResponseNG reply;
    if(!SendCommandNG(CMD_LF_SAMPLING_GET_CONFIG, NULL, 0)) return PM3_EIO;
    if(!WaitForResponseTimeout(CMD_LF_SAMPLING_GET_CONFIG, &reply, LF_CONFIG_TIMEOUT_MS)) {
        return PM3_ETIMEOUT;
    }
    if(reply.status != PM3_SUCCESS) return reply.status;
    if(reply.length != sizeof(*config)) return PM3_ESOFT;
    memcpy(config, reply.data.asBytes, sizeof(*config));
    return PM3_SUCCESS;
}

static bool same_sampling_config(const sample_config* left, const sample_config* right) {
    return memcmp(left, right, offsetof(sample_config, verbose)) == 0;
}

static int set_sampling_config(const sample_config* config) {
    // The verbose flag requests debug replies and is not stored by SET_CONFIG.
    sample_config request = *config;
    request.verbose = false;
    if(!SendCommandNG(CMD_LF_SAMPLING_SET_CONFIG, (uint8_t*)&request, sizeof(request))) {
        return PM3_EIO;
    }
    // SET_CONFIG has no reply. Wait for a fresh echo before verifying it.
    if(!proxmark5_frame_synchronize()) return PM3_ETIMEOUT;
    sample_config actual;
    int status = get_sampling_config(&actual);
    if(status != PM3_SUCCESS) return status;
    return same_sampling_config(&actual, config) ? PM3_SUCCESS : PM3_ESOFT;
}

static int capture_samples(uint32_t count) {
    // 30-bit sample count followed by realtime, verbose and Cotag flags.
    struct {
        uint32_t count;
        uint8_t cotag;
    } PACKED request = {.count = count};
    PacketResponseNG reply;
    if(!SendCommandNG(CMD_LF_ACQ_RAW_ADC, (uint8_t*)&request, sizeof(request))) return PM3_EIO;
    if(!WaitForResponseTimeout(CMD_LF_ACQ_RAW_ADC, &reply, LF_CAPTURE_TIMEOUT_MS)) {
        return PM3_ETIMEOUT;
    }
    if(reply.status != PM3_SUCCESS) return reply.status;
    if(reply.length != sizeof(uint32_t)) return PM3_ESOFT;
    return reply.data.asDwords[0] == 8U * count ? PM3_SUCCESS : PM3_EPARTIAL;
}

int lf_capture_samples(uint32_t count) {
    sample_config previous;
    int status = get_sampling_config(&previous);
    if(status != PM3_SUCCESS) return status;

    sample_config config = previous;
    config.decimation = 1;
    config.bits_per_sample = 8;
    config.divisor = LF_CARRIER_DIVISOR;
    config.trigger_threshold = 0;
    config.samples_to_skip = 0;
    const bool changed = !same_sampling_config(&config, &previous);
    if(changed) status = set_sampling_config(&config);
    if(status == PM3_SUCCESS) status = capture_samples(count);
    if(changed) {
        const volatile bool* cancel = proxmark5_frame_set_operation_cancel(NULL);
        bool restored = set_sampling_config(&previous) == PM3_SUCCESS ||
                        set_sampling_config(&previous) == PM3_SUCCESS;
        proxmark5_frame_set_operation_cancel(cancel);
        if(!restored) return LfCaptureRestoreFailed;
    }
    return proxmark5_frame_operation_cancelled() ? PM3_EOPABORTED : status;
}
