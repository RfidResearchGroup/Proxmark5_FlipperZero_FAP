#include <furi.h>
#include <nfc/helpers/iso14443_crc.h>
#include "hardware_rf.h"
#include "lf_capture.h"
#include "proxmark5_frame.h"

#define RF_COMMAND_TIMEOUT_MS 1500U
#define RF_TYPE2_TIMEOUT_ETU 2100U
#define RF_TYPE2_READ_BYTES 16U

int hardware_rf_threshold(bool apply, uint8_t* value, const volatile bool* cancel) {
    if(apply && (*value < HARDWARE_RF_THRESHOLD_MIN || *value > HARDWARE_RF_THRESHOLD_MAX)) {
        return PM3_EINVARG;
    }
    PacketResponseNG reply;
    if(!SendCommandNG(CMD_HF_ISO14443A_RX_THRESHOLD, apply ? value : NULL, apply ? 1 : 0)) {
        return PM3_EIO;
    }
    int status = proxmark5_frame_wait(CMD_HF_ISO14443A_RX_THRESHOLD, &reply, RF_COMMAND_TIMEOUT_MS, cancel);
    if(status == PM3_ETIMEOUT && proxmark5_frame_synchronize()) return PM3_EDEVNOTSUPP;
    if(status != PM3_SUCCESS) return status;
    if(reply.status != PM3_SUCCESS) return reply.status;
    if(reply.length != 1 || reply.data.asBytes[0] > HARDWARE_RF_THRESHOLD_MAX ||
       (apply && reply.data.asBytes[0] != *value)) {
        return PM3_ESOFT;
    }
    *value = reply.data.asBytes[0];
    return PM3_SUCCESS;
}

int hardware_rf_lf_divisor(bool apply, uint16_t* divisor) {
    sample_config config;
    int status = lf_sampling_get_config(&config);
    if(status != PM3_SUCCESS) return status;
    if(!apply) {
        if(config.divisor < 19 || config.divisor > 255) return PM3_ESOFT;
        *divisor = config.divisor;
        return PM3_SUCCESS;
    }
    if(config.divisor == *divisor) return PM3_SUCCESS;
    config.divisor = *divisor;
    return lf_sampling_set_config(&config);
}

static int hf_field_off(void) {
    const volatile bool* cancel = proxmark5_frame_set_operation_cancel(NULL);
    // DROP FIELD has no response. Synchronize after sending it.
    bool off = SendCommandNG(CMD_HF_DROPFIELD, NULL, 0) && proxmark5_frame_synchronize();
    proxmark5_frame_set_operation_cancel(cancel);
    return off ? PM3_SUCCESS : HardwareRfFieldOffUnconfirmed;
}

static int hf_exchange(void* request, size_t length, PacketResponseNG* reply, const volatile bool* cancel) {
    if(!SendCommandNG(CMD_HF_ISO14443A_READER, request, length)) return PM3_EIO;
    int status = proxmark5_frame_wait(CMD_HF_ISO14443A_READER, reply, RF_COMMAND_TIMEOUT_MS, cancel);
    return status == PM3_SUCCESS ? reply->status : status;
}

static int hf_select(PacketResponseNG* reply, bool* type2, const volatile bool* cancel) {
    iso14a_raw_cmd_t request = {
        .flags = ISO14A_CONNECT | ISO14A_NO_DISCONNECT | ISO14A_NO_RATS,
    };
    int status = hf_exchange(&request, sizeof(request), reply, cancel);
    if(status != PM3_SUCCESS) return status;
    if(reply->length != sizeof(iso14a_raw_resp_t) + sizeof(iso14a_card_select_t)) return PM3_ESOFT;
    const iso14a_raw_resp_t* result = (const iso14a_raw_resp_t*)reply->data.asBytes;
    if(result->len != sizeof(iso14a_card_select_t)) return PM3_ESOFT;
    if(!result->sel) return PM3_ENODATA;
    const iso14a_card_select_t* card = (const iso14a_card_select_t*)result->data;
    *type2 = card->uidlen == 7 && card->sak == 0;
    return PM3_SUCCESS;
}

static int hf_check_crc(const uint8_t* command, size_t size, size_t data_size, PacketResponseNG* reply, const volatile bool* cancel) {
    uint8_t buffer[sizeof(iso14a_raw_cmd_t) + 2] = {0};
    iso14a_raw_cmd_t* request = (iso14a_raw_cmd_t*)buffer;
    request->flags = ISO14A_RAW | ISO14A_APPEND_CRC | ISO14A_NO_DISCONNECT | ISO14A_SET_TIMEOUT;
    request->timeout = RF_TYPE2_TIMEOUT_ETU;
    request->len = size;
    memcpy(request->data, command, size);
    int status = hf_exchange(buffer, sizeof(*request) + size, reply, cancel);
    if(status != PM3_SUCCESS) return status;
    size_t frame_size = data_size + ISO14443_CRC_SIZE;
    if(reply->length != sizeof(iso14a_raw_resp_t) + frame_size) return PM3_ENODATA;
    const iso14a_raw_resp_t* result = (const iso14a_raw_resp_t*)reply->data.asBytes;
    if(result->sel || result->len != frame_size) return PM3_ESOFT;
    BitBuffer* frame = bit_buffer_alloc(frame_size);
    bit_buffer_copy_bytes(frame, result->data, frame_size);
    bool valid = iso14443_crc_check(Iso14443CrcTypeA, frame);
    bit_buffer_free(frame);
    return valid ? PM3_SUCCESS : PM3_ECRC;
}

int hardware_rf_read_test(HardwareRfTest* test, const volatile bool* cancel) {
    PacketResponseNG* reply = malloc(sizeof(PacketResponseNG));
    if(!reply) return PM3_EMALLOC;
    int status = PM3_SUCCESS;
    for(unsigned i = 0; i < HARDWARE_RF_TEST_ATTEMPTS; i++) {
        if(proxmark5_frame_operation_cancelled()) {
            status = PM3_EOPABORTED;
            break;
        }
        bool type2 = false;
        status = hf_select(reply, &type2, cancel);
        if(status == PM3_SUCCESS) {
            test->selected++;
            if(type2) {
                const uint8_t version[] = {0x60};
                const uint8_t read[] = {0x30, 4};
                if(hf_check_crc(version, sizeof(version), 8, reply, cancel) == PM3_SUCCESS) test->version++;
                if(!proxmark5_frame_operation_cancelled() &&
                   hf_check_crc(read, sizeof(read), RF_TYPE2_READ_BYTES, reply, cancel) == PM3_SUCCESS) {
                    test->read++;
                }
            }
        }
        int off = hf_field_off();
        if(off != PM3_SUCCESS) {
            status = off;
            break;
        }
        if(status != PM3_SUCCESS && status != PM3_ENODATA) break;
        status = PM3_SUCCESS;
    }
    free(reply);
    if(status == PM3_SUCCESS && proxmark5_frame_operation_cancelled()) return PM3_EOPABORTED;
    return status;
}
