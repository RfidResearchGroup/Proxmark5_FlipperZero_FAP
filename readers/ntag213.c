#include <furi.h>
#include <nfc/helpers/iso14443_crc.h>
#include "../pages/saved_ntag213.h"
#include "ntag213.h"
#include "proxmark5_frame.h"
#include "simple_readers.h"
#include "type2_ndef.h"

#define NTAG21X_USER_START 4U
#define NTAG21X_MAX_USER_BYTES ((NTAG21X_MAX_READ_PAGES - NTAG21X_USER_START) * NTAG21X_PAGE_BYTES)
#define NTAG21X_NDEF_DISPLAY_SIZE (NTAG21X_MAX_USER_BYTES + 32U) // Space for the URI prefix.
#define NTAG21X_TIMEOUT_MS 2500U
#define NTAG21X_SELECT_ATTEMPTS 10U
#define NTAG21X_VERSION_TIMEOUT_ETU 2100U // About 20 ms.
#define NTAG21X_FAST_READ_PAGES 42U
#define NTAG21X_FAST_READ_ATTEMPTS 4U

static const Ntag21xModel models[] = {
    {"NTAG213", {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x0f, 0x03}, 40, 45},
    {"NTAG215", {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03}, 130, 135},
    {"NTAG216", {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x13, 0x03}, NTAG21X_MAX_READ_PAGES, 231},
};

const Ntag21xModel* ntag21x_model_for_version(const uint8_t version[NTAG21X_VERSION_BYTES]) {
    for(size_t i = 0; i < COUNT_OF(models); i++) {
        if(memcmp(version, models[i].version, NTAG21X_VERSION_BYTES) == 0) return &models[i];
    }
    return NULL;
}

void ntag21x_pages_uid(const Ntag21xPages* pages, uint8_t uid[NTAG21X_UID_BYTES]) {
    memcpy(uid, pages->bytes, 3);
    memcpy(uid + 3, pages->bytes + NTAG21X_PAGE_BYTES, 4);
}

bool ntag21x_pages_valid(const Ntag21xPages* pages) {
    const uint8_t* b = pages->bytes;
    return b[3] == (0x88 ^ b[0] ^ b[1] ^ b[2]) && b[8] == (b[4] ^ b[5] ^ b[6] ^ b[7]);
}

static int ntag21x_link_error(int status) {
    return proxmark5_frame_operation_cancelled() ? PM3_EOPABORTED : status;
}

static int ntag21x_transceive(void* request, size_t length, PacketResponseNG* reply) {
    if(proxmark5_frame_operation_cancelled()) return PM3_EOPABORTED;
    clearCommandBuffer();
    if(!SendCommandNG(CMD_HF_ISO14443A_READER, request, length)) return ntag21x_link_error(PM3_EIO);
    if(!WaitForResponseTimeout(CMD_HF_ISO14443A_READER, reply, NTAG21X_TIMEOUT_MS)) {
        return ntag21x_link_error(PM3_ETIMEOUT);
    }
    return reply->status;
}

static bool ntag21x_crc_valid(const uint8_t* bytes, size_t length) {
    BitBuffer* frame = bit_buffer_alloc(length);
    bit_buffer_copy_bytes(frame, bytes, length);
    bool valid = iso14443_crc_check(Iso14443CrcTypeA, frame);
    bit_buffer_free(frame);
    return valid;
}

static int ntag21x_raw(
    const uint8_t* data, size_t length, uint32_t timeout_etu, PacketResponseNG* reply) {
    uint8_t request[sizeof(iso14a_raw_cmd_t) + 2 + NTAG21X_PAGE_BYTES] = {0};
    furi_assert(length <= sizeof(request) - sizeof(iso14a_raw_cmd_t));
    iso14a_raw_cmd_t* command = (iso14a_raw_cmd_t*)request;
    command->flags = ISO14A_RAW | ISO14A_APPEND_CRC | ISO14A_NO_DISCONNECT;
    if(timeout_etu) command->flags |= ISO14A_SET_TIMEOUT;
    command->timeout = timeout_etu;
    command->len = length;
    memcpy(command->data, data, length);
    int status = ntag21x_transceive(request, sizeof(*command) + length, reply);
    if(status != PM3_SUCCESS) return status;
    if(reply->length < sizeof(iso14a_raw_resp_t)) return PM3_ESOFT;
    const iso14a_raw_resp_t* response = (const iso14a_raw_resp_t*)reply->data.asBytes;
    if(response->sel || response->len != reply->length - sizeof(*response)) return PM3_ESOFT;
    return PM3_SUCCESS;
}

static int ntag21x_select_once(
    uint8_t uid[NTAG21X_UID_BYTES], const Ntag21xModel** model, PacketResponseNG* reply) {
    iso14a_raw_cmd_t command = {
        .flags = ISO14A_CONNECT | ISO14A_NO_RATS | ISO14A_CLEARTRACE | ISO14A_NO_DISCONNECT,
    };
    int status = ntag21x_transceive(&command, sizeof(command), reply);
    if(status != PM3_SUCCESS) return status;
    if(reply->length != sizeof(iso14a_raw_resp_t) + sizeof(iso14a_card_select_t)) return PM3_ESOFT;
    const iso14a_raw_resp_t* response = (const iso14a_raw_resp_t*)reply->data.asBytes;
    if(response->len != sizeof(iso14a_card_select_t)) return PM3_ESOFT;
    if(!response->sel) return PM3_ENODATA;
    const iso14a_card_select_t* card = (const iso14a_card_select_t*)response->data;
    if(card->uidlen != NTAG21X_UID_BYTES || card->sak) return PM3_ENODATA;
    memcpy(uid, card->uid, NTAG21X_UID_BYTES);

    const uint8_t get_version[] = {0x60};
    status = ntag21x_raw(get_version, sizeof(get_version), NTAG21X_VERSION_TIMEOUT_ETU, reply);
    if(status != PM3_SUCCESS) return status;
    response = (const iso14a_raw_resp_t*)reply->data.asBytes;
    if(response->len != NTAG21X_VERSION_BYTES + ISO14443_CRC_SIZE) return PM3_ENODATA;
    if(!ntag21x_crc_valid(response->data, response->len)) return PM3_ECRC;
    *model = ntag21x_model_for_version(response->data);
    return *model ? PM3_SUCCESS : PM3_EDEVNOTSUPP;
}

static int ntag21x_select(
    uint8_t uid[NTAG21X_UID_BYTES], const Ntag21xModel** model, PacketResponseNG* reply) {
    int status = PM3_ENODATA;
    for(unsigned attempt = 0; attempt < NTAG21X_SELECT_ATTEMPTS; attempt++) {
        status = ntag21x_select_once(uid, model, reply);
        if(status == PM3_EDEVNOTSUPP) return PM3_ENODATA;
        if(status != PM3_ENODATA && status != PM3_ECRC) break;
    }
    return status;
}

static int ntag21x_fast_read(unsigned page_count, uint8_t* output, PacketResponseNG* reply) {
    for(unsigned first = 0; first < page_count; first += NTAG21X_FAST_READ_PAGES) {
        unsigned count = MIN(NTAG21X_FAST_READ_PAGES, page_count - first);
        const uint8_t request[] = {0x3a, first, first + count - 1};
        size_t size = count * NTAG21X_PAGE_BYTES;
        size_t frame_size = size + ISO14443_CRC_SIZE;
        int status = PM3_EPARTIAL;
        for(unsigned attempt = 0; attempt < NTAG21X_FAST_READ_ATTEMPTS; attempt++) {
            status = ntag21x_raw(request, sizeof(request), 0, reply);
            if(status != PM3_SUCCESS) return status;
            const iso14a_raw_resp_t* response = (const iso14a_raw_resp_t*)reply->data.asBytes;
            if(response->len != frame_size) {
                status = PM3_EPARTIAL;
                continue;
            }
            if(!ntag21x_crc_valid(response->data, frame_size)) {
                status = PM3_ECRC;
                continue;
            }
            memcpy(output + first * NTAG21X_PAGE_BYTES, response->data, size);
            break;
        }
        if(status != PM3_SUCCESS) return status;
    }
    return PM3_SUCCESS;
}

static bool ntag21x_field_off(void) {
    // Field shutdown has no reply and must finish even when the scan is cancelled.
    const volatile bool* cancel = proxmark5_frame_set_operation_cancel(NULL);
    iso14a_raw_cmd_t command = {0};
    bool off = SendCommandNG(CMD_HF_ISO14443A_READER, (uint8_t*)&command, sizeof(command)) &&
               proxmark5_frame_synchronize();
    proxmark5_frame_set_operation_cancel(cancel);
    return off;
}

static const char* ntag21x_format_ndef(
    const uint8_t* pages, const Ntag21xModel* model, char* details, size_t size) {
    details[0] = '\0';
    const uint8_t* capability = pages + 3 * NTAG21X_PAGE_BYTES;
    if(capability[0] != 0xe1 || capability[1] >> 4 != 1) return "NDEF: unformatted";
    size_t data_size = capability[2] * 8U;
    size_t user_size = (model->read_pages - NTAG21X_USER_START) * NTAG21X_PAGE_BYTES;
    if(!data_size || data_size > user_size) return "NDEF: invalid size";
    if(capability[3] >> 4) return "NDEF: restricted";

    const uint8_t* data = pages + NTAG21X_USER_START * NTAG21X_PAGE_BYTES;
    Type2NdefResult status = type2_ndef_read(data, data_size, details, size);
    if(status == Type2NdefText) return "NDEF text";
    if(status == Type2NdefUri) return "NDEF URL";
    if(status == Type2NdefTextAndUri) return "NDEF text + URL";
    if(status == Type2NdefInvalid) return "NDEF: invalid";
    return status == Type2NdefEmpty ? "NDEF: empty" : "NDEF: unsupported";
}

static void ntag21x_format_result(SimpleReaderResult* result) {
    const Ntag21xPages* pages = (const Ntag21xPages*)result->data;
    const Ntag21xModel* model = ntag21x_model_for_version(pages->version);
    furi_assert(model);
    uint8_t uid[NTAG21X_UID_BYTES];
    ntag21x_pages_uid(pages, uid);
    snprintf(
        result->lines[0],
        sizeof(result->lines[0]),
        "UID %02X%02X%02X%02X%02X%02X%02X",
        uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
    unsigned user_size = (model->read_pages - NTAG21X_USER_START) * NTAG21X_PAGE_BYTES;
    snprintf(result->lines[1], sizeof(result->lines[1]), "%s  %u bytes", model->name, user_size);
    char text[NTAG21X_NDEF_DISPLAY_SIZE];
    const char* ndef = ntag21x_format_ndef(pages->bytes, model, text, sizeof(text));
    snprintf(result->lines[2], sizeof(result->lines[2]), "%s", ndef);
    result->line_count = 3;
}

static void ntag21x_format_details(const uint8_t* data, FuriString* text) {
    const Ntag21xPages* pages = (const Ntag21xPages*)data;
    const Ntag21xModel* model = ntag21x_model_for_version(pages->version);
    furi_assert(model);
    char ndef[NTAG21X_NDEF_DISPLAY_SIZE];
    const char* label = ntag21x_format_ndef(pages->bytes, model, ndef, sizeof(ndef));
    furi_string_reset(text);
    if(*ndef) furi_string_printf(text, "%s\n%s\n\n", label, ndef);
    furi_string_cat_printf(text, "User pages 04-%u\n", model->read_pages - 1U);
    for(unsigned page = NTAG21X_USER_START; page < model->read_pages; page++) {
        const uint8_t* bytes = pages->bytes + page * NTAG21X_PAGE_BYTES;
        furi_string_cat_printf(
            text, "%02u  %02X%02X%02X%02X\n", page, bytes[0], bytes[1], bytes[2], bytes[3]);
    }
}

static int ntag21x_read(SimpleReaderResult* result) {
    Ntag21xPages* pages = (Ntag21xPages*)result->data;
    uint8_t uid[NTAG21X_UID_BYTES];
    PacketResponseNG reply;
    const Ntag21xModel* model = NULL;
    int status = ntag21x_select(uid, &model, &reply);
    if(status == PM3_SUCCESS) {
        memset(pages, 0, sizeof(*pages));
        status = ntag21x_fast_read(model->read_pages, pages->bytes, &reply);
    }
    bool field_off = ntag21x_field_off();
    if(proxmark5_frame_operation_cancelled()) return PM3_EOPABORTED;
    if(status != PM3_SUCCESS) return status;
    if(!field_off) return PM3_EIO;
    if(!ntag21x_pages_valid(pages)) return PM3_ESOFT;
    uint8_t read_uid[NTAG21X_UID_BYTES];
    ntag21x_pages_uid(pages, read_uid);
    if(memcmp(uid, read_uid, sizeof(uid))) {
        return PM3_ECARDEXCHANGE;
    }

    memcpy(pages->version, model->version, sizeof(pages->version));
    ntag21x_format_result(result);
    return PM3_SUCCESS;
}

static void ntag21x_save(const uint8_t* data) {
    saved_ntag21x_save((const Ntag21xPages*)data);
}

const SimpleReader simple_reader_ntag21x = {
    .title = "NTAG21x Reader",
    .prompt = "Place NTAG21x on HF coil",
    .no_data = "No NTAG21x found",
    .no_data_hint = "Use the PM5 HF antenna",
    .data_size = sizeof(Ntag21xPages),
    .read = ntag21x_read,
    .save = ntag21x_save,
    .format = ntag21x_format_result,
    .format_details = ntag21x_format_details,
};
