#include <furi.h>
#include <nfc/helpers/iso14443_crc.h>
#include "bigbuf.h"
#include "ntag213.h"
#include "proxmark5_frame.h"
#include "simple_readers.h"
#include "type2_ndef.h"

#define NTAG213_USER_START 4U
#define NTAG213_USER_BYTES ((NTAG213_READ_PAGES - NTAG213_USER_START) * NTAG213_PAGE_BYTES)
#define NTAG213_TIMEOUT_MS 2500U
const uint8_t ntag213_version[NTAG213_VERSION_BYTES] = {
    0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x0f, 0x03};

void ntag213_pages_uid(const Ntag213Pages* pages, uint8_t uid[NTAG213_UID_BYTES]) {
    memcpy(uid, pages->bytes, 3);
    memcpy(uid + 3, pages->bytes + NTAG213_PAGE_BYTES, 4);
}

bool ntag213_pages_valid(const Ntag213Pages* pages) {
    const uint8_t* b = pages->bytes;
    return b[3] == (0x88 ^ b[0] ^ b[1] ^ b[2]) && b[8] == (b[4] ^ b[5] ^ b[6] ^ b[7]);
}

static int read_reply(uint16_t cmd, PacketResponseNG* reply) {
    if(!WaitForResponseTimeout(cmd, reply, NTAG213_TIMEOUT_MS)) return PM3_ETIMEOUT;
    return reply->status;
}

static int ntag213_select(uint8_t uid[NTAG213_UID_BYTES], PacketResponseNG* reply) {
    uint8_t request[sizeof(iso14a_raw_cmd_t) + 1] = {0};
    iso14a_raw_cmd_t* command = (iso14a_raw_cmd_t*)request;
    command->flags = ISO14A_CONNECT | ISO14A_RAW | ISO14A_APPEND_CRC |
                     ISO14A_NO_RATS | ISO14A_CLEARTRACE;
    command->len = 1;
    command->data[0] = 0x60; // GET_VERSION
    clearCommandBuffer();
    if(!SendCommandNG(CMD_HF_ISO14443A_READER, request, sizeof(request))) {
        return PM3_EIO;
    }
    int status = read_reply(CMD_HF_ISO14443A_READER, reply);
    if(status != PM3_SUCCESS) return status;
    if(reply->length != sizeof(iso14a_raw_resp_t) + sizeof(iso14a_card_select_t)) {
        return PM3_ESOFT;
    }

    const iso14a_raw_resp_t* response = (const iso14a_raw_resp_t*)reply->data.asBytes;
    if(!response->sel) return PM3_ENODATA;
    if(response->len != sizeof(iso14a_card_select_t)) return PM3_ESOFT;
    const iso14a_card_select_t* card = (const iso14a_card_select_t*)response->data;
    bool selected_type2 = card->uidlen == NTAG213_UID_BYTES && card->sak == 0;
    if(selected_type2) memcpy(uid, card->uid, NTAG213_UID_BYTES);

    // CONNECT and RAW return separate replies with the same command ID.
    status = read_reply(CMD_HF_ISO14443A_READER, reply);
    if(status != PM3_SUCCESS) return status;
    if(reply->length < sizeof(iso14a_raw_resp_t)) return PM3_ESOFT;
    response = (const iso14a_raw_resp_t*)reply->data.asBytes;
    if(response->sel || response->len != reply->length - sizeof(*response)) return PM3_ESOFT;

    if(!selected_type2 || response->len != sizeof(ntag213_version) + ISO14443_CRC_SIZE ||
       memcmp(response->data, ntag213_version, sizeof(ntag213_version)) != 0) {
        return PM3_ENODATA;
    }
    BitBuffer* frame = bit_buffer_alloc(response->len);
    bit_buffer_copy_bytes(frame, response->data, response->len);
    bool crc_ok = iso14443_crc_check(Iso14443CrcTypeA, frame);
    bit_buffer_free(frame);
    return crc_ok ? PM3_SUCCESS : PM3_ECRC;
}

static int ntag213_read_pages(uint8_t* pages, PacketResponseNG* reply) {
    mful_readblock_t request = {.num_of_blocks = NTAG213_READ_PAGES};
    clearCommandBuffer();
    if(!SendCommandNG(CMD_HF_MIFAREU_READCARD, (uint8_t*)&request, sizeof(request))) {
        return PM3_EIO;
    }
    int status = read_reply(CMD_HF_MIFAREU_READCARD, reply);
    if(status != PM3_SUCCESS) return status;
    if(reply->length != sizeof(mful_readblock_resp_t)) return PM3_ESOFT;

    mful_readblock_resp_t range;
    memcpy(&range, reply->data.asBytes, sizeof(range));
    if(range.bytelen < NTAG213_READ_BYTES) return PM3_EPARTIAL;
    if(range.bytelen != NTAG213_READ_BYTES) return PM3_ESOFT;
    return bigbuf_download(pages, range.startidx, range.bytelen);
}

static const char* ntag213_format_text(const uint8_t* pages, char* details, size_t size) {
    const uint8_t* capability = pages + 3 * NTAG213_PAGE_BYTES;
    if(capability[0] != 0xe1 || capability[1] >> 4 != 1) return "NDEF: unformatted";
    size_t data_size = capability[2] * 8U;
    if(!data_size || data_size > NTAG213_USER_BYTES) return "NDEF: invalid size";
    if(capability[3] >> 4) return "NDEF: restricted";

    const uint8_t* data = pages + NTAG213_USER_START * NTAG213_PAGE_BYTES;
    const char prefix[] = "NDEF text\n";
    memcpy(details, prefix, sizeof(prefix));
    Type2NdefResult status =
        type2_ndef_read_text(data, data_size, details + sizeof(prefix) - 1, size - sizeof(prefix) + 1);
    if(status == Type2NdefText) return "NDEF text";
    details[0] = '\0';
    if(status == Type2NdefInvalid) return "NDEF: invalid";
    return status == Type2NdefEmpty ? "NDEF: empty" : "NDEF: unsupported";
}

static void ntag213_format_result(SimpleReaderResult* result) {
    uint8_t uid[NTAG213_UID_BYTES];
    ntag213_pages_uid((const Ntag213Pages*)result->data, uid);
    snprintf(
        result->lines[0],
        sizeof(result->lines[0]),
        "UID %02X%02X%02X%02X%02X%02X%02X",
        uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
    snprintf(result->lines[1], sizeof(result->lines[1]), "%u user bytes", NTAG213_USER_BYTES);
    char text[NTAG213_USER_BYTES + 16];
    const char* ndef = ntag213_format_text(result->data, text, sizeof(text));
    snprintf(result->lines[2], sizeof(result->lines[2]), "%s", ndef);
    result->line_count = 3;
}

static void ntag213_format_details(const uint8_t* pages, FuriString* text) {
    char ndef[NTAG213_USER_BYTES + 16];
    ndef[0] = '\0';
    ntag213_format_text(pages, ndef, sizeof(ndef));
    furi_string_printf(text, "%s%sUser pages 04-39\n", ndef, *ndef ? "\n\n" : "");
    for(unsigned page = NTAG213_USER_START; page < NTAG213_READ_PAGES; page++) {
        const uint8_t* bytes = pages + page * NTAG213_PAGE_BYTES;
        furi_string_cat_printf(
            text, "%02u  %02X%02X%02X%02X\n", page, bytes[0], bytes[1], bytes[2], bytes[3]);
    }
}

static int ntag213_read(SimpleReaderResult* result) {
    uint8_t uid[NTAG213_UID_BYTES];
    PacketResponseNG reply;
    int status = ntag213_select(uid, &reply);
    if(status != PM3_SUCCESS) return status;
    status = ntag213_read_pages(result->data, &reply);
    if(status != PM3_SUCCESS) return status;
    uint8_t read_uid[NTAG213_UID_BYTES];
    ntag213_pages_uid((const Ntag213Pages*)result->data, read_uid);
    if(memcmp(uid, read_uid, sizeof(uid))) {
        return PM3_ECARDEXCHANGE;
    }

    ntag213_format_result(result);
    return PM3_SUCCESS;
}

const SimpleReader simple_reader_ntag213 = {
    .title = "NTAG213 Reader",
    .prompt = "Place NTAG213 on HF coil",
    .no_data = "No NTAG213 found",
    .no_data_hint = "Use the PM5 HF antenna",
    .data_size = NTAG213_READ_BYTES,
    .read = ntag213_read,
    .format_details = ntag213_format_details,
};
