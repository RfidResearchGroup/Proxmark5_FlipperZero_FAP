#include <furi.h>
#include <furi_hal_power.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
#include "classic_dump_page.h"
#include "proxmark5_frame.h"
#include "proxmark5_com.h"
#include "pm3_cmd.h"

#define ISO14A_CONNECT    (1U << 0)
#define ISO14A_CLEARTRACE (1U << 17)

#define CLASSIC_1K_SECTORS 16
#define CLASSIC_1K_BLOCKS  64
#define CLASSIC_BLOCK_SIZE 16
#define CLASSIC_KEY_SIZE   6
#define CHKKEYS_BATCH_MAX  32
#define CHKKEYS_FAST_TIMEOUT_MS 60000U
#define AUTOPWN_PARTIAL 1

typedef enum {
    ClassicDumpStateIdle,
    ClassicDumpStateRunning,
    ClassicDumpStateChoiceDict, /* after dict: Collect / Dump / Back */
    ClassicDumpStateChoiceNonce, /* after nonces: Dump / MFKey later / Back */
    ClassicDumpStateSuccess,
    ClassicDumpStatePartial,
    ClassicDumpStateTimeout,
    ClassicDumpStateError,
} ClassicDumpState;

typedef enum {
    ClassicWorkerJobDict = 0,
    ClassicWorkerJobCollect,
    ClassicWorkerJobDump,
} ClassicWorkerJob;

typedef struct {
    ClassicDumpState state;
    char line1[48];
    char line2[48];
    char line3[48];
    uint8_t sectors_ok;
    uint8_t sectors_total;
    bool can_save;
    bool need_hardnested;
} ClassicDumpPageModel;

struct ClassicDumpPage {
    View* view;
    FuriThread* worker_thread;
    bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;
    ClassicWorkerJob worker_job;
    ClassicDumpSaveRequestCallback save_request_callback;
    void* save_request_context;
    ClassicDumpBackRequestCallback back_request_callback;
    void* back_request_context;
    ClassicDumpMfkeyRequestCallback mfkey_request_callback;
    void* mfkey_request_context;
    NotificationApp* notifications;
    bool backlight_enforced;

    uint8_t dump[CLASSIC_1K_BLOCKS][CLASSIC_BLOCK_SIZE];
    uint8_t known[CLASSIC_1K_BLOCKS];
    uint8_t key_a[CLASSIC_1K_SECTORS][CLASSIC_KEY_SIZE];
    uint8_t key_b[CLASSIC_1K_SECTORS][CLASSIC_KEY_SIZE];
    uint8_t key_a_ok[CLASSIC_1K_SECTORS];
    uint8_t key_b_ok[CLASSIC_1K_SECTORS];
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t atqa_hi;
    uint8_t atqa_lo;
    uint8_t sak;
    char uid_hex[28];
    bool need_hardnested;
    uint16_t sectors_ok_mask; /* bit s set if sector s dumped OK */
    uint8_t nonces_saved;
    uint8_t missing_ab; /* A/B slots still missing after dict */
};

static ClassicDumpPage* s_classic_dump_page = NULL;

#define DICT_DIR           APP_DATA_PATH("dicts")
#define DICT_SCAN_MAX      16
#define DICT_PATH_MAX      96
#define DICT_LIST_MAX      16
/* Skip mega-dicts (6_byte_* etc): ~4 keys/s → hours, never nested-class keys */
#define DICT_MAX_BYTES     (128U * 1024U)
#define NESTED_LOG_PATH    EXT_PATH("nfc/.nested.log")
#define NESTED_MIRROR_PATH APP_DATA_PATH("nested_last.log")
#define STATIC_NESTED_TIMEOUT_MS 5000U

/* Factory / MAD / NDEF keys — enough for easy cards without SD */
static const uint8_t k_builtin_keys[][CLASSIC_KEY_SIZE] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},
    {0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF1},
    {0xA1, 0xB1, 0xC1, 0xD1, 0xE1, 0xF0},
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97},
    {0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F},
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA6},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB6},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE},
};
#define BUILTIN_KEY_COUNT (sizeof(k_builtin_keys) / sizeof(k_builtin_keys[0]))

/* Filled by classic_build_dict_list — paths must outlive the call */
static char s_scanned_dict_paths[DICT_SCAN_MAX][DICT_PATH_MAX];

static bool classic_parse_hex_key(const char* line, uint8_t* out_key);
static uint8_t classic_keys_found_count(const ClassicDumpPage* page);
static void classic_dump_page_start_job(ClassicDumpPage* page, ClassicWorkerJob job);

static void classic_backlight_enforce(ClassicDumpPage* page, bool on) {
    if(!page || !page->notifications) {
        return;
    }
    if(on && !page->backlight_enforced) {
        notification_message(page->notifications, &sequence_display_backlight_enforce_on);
        page->backlight_enforced = true;
    } else if(!on && page->backlight_enforced) {
        notification_message(page->notifications, &sequence_display_backlight_enforce_auto);
        page->backlight_enforced = false;
    }
}

static void classic_otg_keepalive(void) {
    if(furi_hal_power_check_otg_fault() || !furi_hal_power_is_otg_enabled()) {
        furi_hal_power_disable_otg();
        furi_delay_ms(20);
        furi_hal_power_enable_otg();
    }
}

static void classic_ui(
    ClassicDumpPage* page,
    const char* l1,
    const char* l2,
    const char* l3) {
    ClassicDumpPageModel* model = view_get_model(page->view);
    if(l1) snprintf(model->line1, sizeof(model->line1), "%s", l1);
    if(l2) snprintf(model->line2, sizeof(model->line2), "%s", l2);
    if(l3) {
        snprintf(model->line3, sizeof(model->line3), "%s", l3);
    } else {
        snprintf(
            model->line3,
            sizeof(model->line3),
            "keys %u/32",
            classic_keys_found_count(page));
    }
    view_commit_model(page->view, false);
}

static int classic_wait(
    uint16_t cmd,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel) {
    uint32_t start = furi_get_tick();
    uint32_t last_bl = start;
    while((furi_get_tick() - start) < timeout_ms) {
        if(cancel && *cancel) {
            SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
            return PM3_EOPABORTED;
        }
        if(proxmark5_frame_take_by_cmd(cmd, resp)) {
            return PM3_SUCCESS;
        }
        if(s_classic_dump_page && (furi_get_tick() - last_bl) > 5000) {
            classic_backlight_enforce(s_classic_dump_page, true);
            classic_otg_keepalive();
            last_bl = furi_get_tick();
        }
        furi_delay_ms(10);
    }
    return proxmark5_frame_take_by_cmd(cmd, resp) ? PM3_SUCCESS : PM3_ETIMEOUT;
}

static bool classic_sanitize_name(const char* input, char* output, size_t output_size) {
    if(!input || !output || output_size < 2) {
        return false;
    }
    size_t out = 0;
    for(size_t i = 0; input[i] && out + 1 < output_size; i++) {
        char c = input[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-') {
            output[out++] = c;
        } else if(c == ' ') {
            output[out++] = '_';
        }
    }
    output[out] = '\0';
    size_t len = strlen(output);
    if(len > 4) {
        const char* ext = output + len - 4;
        if(ext[0] == '.' && (ext[1] == 'n' || ext[1] == 'N') &&
           (ext[2] == 'f' || ext[2] == 'F') && (ext[3] == 'c' || ext[3] == 'C')) {
            output[len - 4] = '\0';
        }
    }
    return output[0] != '\0';
}

static int classic_select_card(ClassicDumpPage* page, volatile bool* cancel) {
    PacketResponseNG resp;
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT | ISO14A_CLEARTRACE, 0, 0, NULL, 0);
    int rc = classic_wait(CMD_ACK, &resp, 2500, cancel);
    if(rc != PM3_SUCCESS) {
        return rc;
    }
    if(resp.oldarg[0] == 0) {
        return PM3_EFAILED;
    }

    uint8_t uidlen = (uint8_t)resp.oldarg[1];
    if(uidlen == 0 || uidlen > 10) {
        uidlen = (resp.length > 10) ? resp.data.asBytes[10] : 0;
    }
    if(uidlen == 0 || uidlen > 10 || resp.length < uidlen || resp.length < 14) {
        return PM3_ESOFT;
    }

    memcpy(page->uid, resp.data.asBytes, uidlen);
    page->uid_len = uidlen;
    page->atqa_lo = resp.data.asBytes[11];
    page->atqa_hi = resp.data.asBytes[12];
    page->sak = resp.data.asBytes[13];

    size_t pos = 0;
    for(uint8_t i = 0; i < uidlen && pos + 3 < sizeof(page->uid_hex); i++) {
        pos += snprintf(page->uid_hex + pos, sizeof(page->uid_hex) - pos, "%02X", page->uid[i]);
    }
    return PM3_SUCCESS;
}

static bool classic_parse_hex_key(const char* line, uint8_t* out_key) {
    char hex[13];
    size_t n = 0;
    for(size_t i = 0; line[i] != '\0' && n < 12; i++) {
        char c = line[i];
        if(c == '#' || c == '\r' || c == '\n') {
            break;
        }
        if(c == ' ' || c == ':' || c == '-') {
            continue;
        }
        if(c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        if(!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
        hex[n++] = c;
    }
    if(n != 12) {
        return false;
    }
    hex[12] = '\0';
    for(uint8_t i = 0; i < 6; i++) {
        int hi = -1;
        int lo = -1;
        char c0 = hex[i * 2];
        char c1 = hex[i * 2 + 1];
        if(c0 >= '0' && c0 <= '9')
            hi = c0 - '0';
        else if(c0 >= 'A' && c0 <= 'F')
            hi = c0 - 'A' + 10;
        if(c1 >= '0' && c1 <= '9')
            lo = c1 - '0';
        else if(c1 >= 'A' && c1 <= 'F')
            lo = c1 - 'A' + 10;
        if(hi < 0 || lo < 0) {
            return false;
        }
        out_key[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static uint8_t classic_keys_found_count(const ClassicDumpPage* page) {
    uint8_t n = 0;
    for(uint8_t s = 0; s < CLASSIC_1K_SECTORS; s++) {
        if(page->key_a_ok[s]) n++;
        if(page->key_b_ok[s]) n++;
    }
    return n;
}

static bool classic_all_ab_found(const ClassicDumpPage* page) {
    return classic_keys_found_count(page) == (CLASSIC_1K_SECTORS * 2);
}

static bool classic_all_sectors_readable(const ClassicDumpPage* page) {
    for(uint8_t s = 0; s < CLASSIC_1K_SECTORS; s++) {
        if(!page->key_a_ok[s] && !page->key_b_ok[s]) {
            return false;
        }
    }
    return true;
}

static bool classic_name_ends_with_dic(const char* name) {
    size_t len = strlen(name);
    if(len < 4) {
        return false;
    }
    const char* e = name + len - 4;
    return e[0] == '.' && (e[1] == 'd' || e[1] == 'D') && (e[2] == 'i' || e[2] == 'I') &&
           (e[3] == 'c' || e[3] == 'C');
}

static bool classic_path_in_list(const char** paths, size_t n, const char* path) {
    for(size_t i = 0; i < n; i++) {
        if(strcmp(paths[i], path) == 0) {
            return true;
        }
    }
    return false;
}

static bool classic_dict_too_large(const FileInfo* info, const char* name) {
    if(strncmp(name, "6_byte_", 7) == 0) {
        return true;
    }
    if(info && info->size > DICT_MAX_BYTES) {
        return true;
    }
    return false;
}

/* Prefer mfc_default_keys, then other small *.dic (skip private_* / laundry_* and huge dumps). */
static void classic_build_dict_list(
    const ClassicDumpPage* page,
    const char** out_paths,
    size_t* out_count,
    size_t max_out) {
    UNUSED(page);
    size_t n = 0;
    size_t scanned = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);

    if(n < max_out && scanned < DICT_SCAN_MAX) {
        snprintf(
            s_scanned_dict_paths[scanned],
            DICT_PATH_MAX,
            "%s/mfc_default_keys.dic",
            DICT_DIR);
        if(storage_file_exists(storage, s_scanned_dict_paths[scanned])) {
            out_paths[n++] = s_scanned_dict_paths[scanned];
            scanned++;
        }
    }

    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, DICT_DIR)) {
        FileInfo info;
        char name[64];
        while(n < max_out && scanned < DICT_SCAN_MAX &&
              storage_dir_read(dir, &info, name, sizeof(name))) {
            if(file_info_is_dir(&info) || !classic_name_ends_with_dic(name)) {
                continue;
            }
            if(strncmp(name, "laundry_", 8) == 0 || strncmp(name, "private_", 8) == 0) {
                continue;
            }
            if(classic_dict_too_large(&info, name)) {
                continue;
            }
            snprintf(s_scanned_dict_paths[scanned], DICT_PATH_MAX, "%s/%s", DICT_DIR, name);
            if(classic_path_in_list(out_paths, n, s_scanned_dict_paths[scanned])) {
                continue;
            }
            out_paths[n++] = s_scanned_dict_paths[scanned];
            scanned++;
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    *out_count = n;
}

static int classic_chkkeys_ng(
    uint8_t block,
    uint8_t key_type,
    const uint8_t* keys,
    uint8_t key_count,
    uint8_t* out_key,
    volatile bool* cancel) {
    if(key_count == 0 || key_count > CHKKEYS_BATCH_MAX) {
        return PM3_EINVARG;
    }
    uint8_t data[5 + CHKKEYS_BATCH_MAX * CLASSIC_KEY_SIZE];
    data[0] = key_type;
    data[1] = block;
    data[2] = 1;
    data[3] = 0;
    data[4] = key_count;
    memcpy(data + 5, keys, (size_t)key_count * CLASSIC_KEY_SIZE);

    PacketResponseNG resp;
    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_CHKKEYS, data, 5 + (size_t)key_count * CLASSIC_KEY_SIZE);
    proxmark5_com_expect_rx(8000);
    int rc = classic_wait(CMD_HF_MIFARE_CHKKEYS, &resp, 8000, cancel);
    if(rc != PM3_SUCCESS) {
        return rc;
    }
    if(resp.status != PM3_SUCCESS || resp.length < 7) {
        return PM3_ESOFT;
    }
    if(!resp.data.asBytes[6]) {
        return PM3_ESOFT;
    }
    memcpy(out_key, resp.data.asBytes, CLASSIC_KEY_SIZE);
    return PM3_SUCCESS;
}

static void classic_store_key(ClassicDumpPage* page, uint8_t sector, uint8_t kt, const uint8_t* key) {
    if(kt == 0) {
        memcpy(page->key_a[sector], key, CLASSIC_KEY_SIZE);
        page->key_a_ok[sector] = 1;
    } else {
        memcpy(page->key_b[sector], key, CLASSIC_KEY_SIZE);
        page->key_b_ok[sector] = 1;
    }
}

/* Reuse one found key across all missing sector/keytypes ('R' like autopwn) */
static int classic_reuse_key(
    ClassicDumpPage* page,
    const uint8_t* key,
    volatile bool* cancel) {
    for(uint8_t s = 0; s < CLASSIC_1K_SECTORS; s++) {
        if(cancel && *cancel) return PM3_EOPABORTED;
        uint8_t first = (uint8_t)(s * 4);
        for(uint8_t kt = 0; kt < 2; kt++) {
            if((kt == 0 && page->key_a_ok[s]) || (kt == 1 && page->key_b_ok[s])) {
                continue;
            }
            uint8_t out[CLASSIC_KEY_SIZE];
            if(classic_chkkeys_ng(first, kt, key, 1, out, cancel) == PM3_SUCCESS) {
                classic_store_key(page, s, kt, out);
            }
        }
    }
    return PM3_SUCCESS;
}

static int classic_read_block(
    uint8_t block,
    uint8_t key_type,
    const uint8_t* key,
    uint8_t* out16,
    volatile bool* cancel) {
    mf_readblock_t payload;
    payload.blockno = block;
    payload.keytype = key_type;
    memcpy(payload.key, key, CLASSIC_KEY_SIZE);

    PacketResponseNG resp;
    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t*)&payload, sizeof(payload));
    int rc = classic_wait(CMD_HF_MIFARE_READBL, &resp, 2000, cancel);
    if(rc != PM3_SUCCESS) {
        return rc;
    }
    if(resp.status != PM3_SUCCESS || resp.length < CLASSIC_BLOCK_SIZE) {
        return PM3_ESOFT;
    }
    memcpy(out16, resp.data.asBytes, CLASSIC_BLOCK_SIZE);
    return PM3_SUCCESS;
}

/* If key A works, try to pull key B from trailer (ACL permitting) */
static void classic_try_read_key_b_from_trailer(
    ClassicDumpPage* page,
    uint8_t sector,
    volatile bool* cancel) {
    if(!page->key_a_ok[sector] || page->key_b_ok[sector]) {
        return;
    }
    uint8_t trailer = (uint8_t)(sector * 4 + 3);
    uint8_t raw[CLASSIC_BLOCK_SIZE];
    if(classic_read_block(trailer, 0, page->key_a[sector], raw, cancel) != PM3_SUCCESS) {
        return;
    }
    /* Trailer layout: A[0..5] ACL[6..9] B[10..15] — B readable if ACL allows */
    bool b_nonzero = false;
    for(uint8_t i = 10; i < 16; i++) {
        if(raw[i] != 0x00) b_nonzero = true;
    }
    if(b_nonzero) {
        classic_store_key(page, sector, 1, raw + 10);
    }
}

static int classic_chk_batch(
    ClassicDumpPage* page,
    const char* ui_label,
    const uint8_t* keys,
    uint8_t key_count,
    volatile bool* cancel) {
    if(key_count == 0) {
        return PM3_SUCCESS;
    }
    for(uint8_t s = 0; s < CLASSIC_1K_SECTORS; s++) {
        if(cancel && *cancel) {
            return PM3_EOPABORTED;
        }
        if(page->key_a_ok[s] && page->key_b_ok[s]) {
            continue;
        }
        char secbuf[24];
        snprintf(secbuf, sizeof(secbuf), "Sector %u/16", s + 1);
        classic_ui(page, ui_label, secbuf, NULL);

        uint8_t first = (uint8_t)(s * 4);
        for(uint8_t kt = 0; kt < 2; kt++) {
            if((kt == 0 && page->key_a_ok[s]) || (kt == 1 && page->key_b_ok[s])) {
                continue;
            }
            uint8_t out[CLASSIC_KEY_SIZE];
            if(classic_chkkeys_ng(first, kt, keys, key_count, out, cancel) == PM3_SUCCESS) {
                classic_store_key(page, s, kt, out);
                classic_reuse_key(page, out, cancel);
                if(kt == 0) {
                    classic_try_read_key_b_from_trailer(page, s, cancel);
                }
            }
        }
        if(classic_all_sectors_readable(page)) {
            return PM3_SUCCESS;
        }
    }
    return PM3_SUCCESS;
}

static int classic_builtin_phase(ClassicDumpPage* page, volatile bool* cancel) {
    classic_ui(page, "Autopwn dict...", "builtin", NULL);
    return classic_chk_batch(
        page,
        "Autopwn dict...",
        &k_builtin_keys[0][0],
        (uint8_t)BUILTIN_KEY_COUNT,
        cancel);
}

static int classic_dict_phase(ClassicDumpPage* page, volatile bool* cancel) {
    const char* paths[DICT_LIST_MAX];
    size_t path_count = 0;
    classic_build_dict_list(page, paths, &path_count, DICT_LIST_MAX);
    if(path_count == 0) {
        return classic_keys_found_count(page) > 0 ? PM3_SUCCESS : PM3_EFILE;
    }

    for(size_t di = 0; di < path_count; di++) {
        if(cancel && *cancel) return PM3_EOPABORTED;
        if(classic_all_ab_found(page)) return PM3_SUCCESS;

        classic_otg_keepalive();

        const char* path = paths[di];
        const char* base = strrchr(path, '/');
        classic_ui(page, "Autopwn dict...", base ? base + 1 : "dict", NULL);

        Storage* storage = furi_record_open(RECORD_STORAGE);
        File* file = storage_file_alloc(storage);
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            storage_file_free(file);
            furi_record_close(RECORD_STORAGE);
            continue;
        }

        uint8_t batch[CHKKEYS_BATCH_MAX][CLASSIC_KEY_SIZE];
        uint8_t batch_count = 0;
        char line[96];
        size_t line_len = 0;

        while(true) {
            if(cancel && *cancel) {
                storage_file_close(file);
                storage_file_free(file);
                furi_record_close(RECORD_STORAGE);
                return PM3_EOPABORTED;
            }

            uint8_t ch = 0;
            size_t rd = storage_file_read(file, &ch, 1);
            bool eof = (rd == 0);
            if(!eof && ch != '\n' && ch != '\r') {
                if(line_len + 1 < sizeof(line)) {
                    line[line_len++] = (char)ch;
                } else {
                    line_len = 0;
                }
                continue;
            }
            if(line_len > 0) {
                line[line_len] = '\0';
                line_len = 0;
                char* p = line;
                while(*p == ' ' || *p == '\t') p++;
                uint8_t key[CLASSIC_KEY_SIZE];
                if(*p && *p != '#' && classic_parse_hex_key(p, key)) {
                    memcpy(batch[batch_count++], key, CLASSIC_KEY_SIZE);
                }
            }

            if(batch_count == CHKKEYS_BATCH_MAX || (eof && batch_count > 0)) {
                int brc = classic_chk_batch(
                    page, "Autopwn dict...", &batch[0][0], batch_count, cancel);
                batch_count = 0;
                if(brc == PM3_EOPABORTED) {
                    storage_file_close(file);
                    storage_file_free(file);
                    furi_record_close(RECORD_STORAGE);
                    return PM3_EOPABORTED;
                }
                if(classic_all_sectors_readable(page)) {
                    storage_file_close(file);
                    storage_file_free(file);
                    furi_record_close(RECORD_STORAGE);
                    return PM3_SUCCESS;
                }
            }
            if(eof) break;
        }

        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);

        /* Stop only when every sector is readable — keep scanning remaining
         * dicts for missing sectors (nested is not available on Flipper). */
        if(classic_all_sectors_readable(page)) {
            break;
        }
    }

    return classic_keys_found_count(page) > 0 ? PM3_SUCCESS : PM3_ESOFT;
}

static void classic_set_trailer(
    ClassicDumpPage* page,
    uint8_t sector,
    const uint8_t* acl_src) {
    uint8_t trailer = (uint8_t)(sector * 4 + 3);
    uint8_t* blk = page->dump[trailer];
    memset(blk, 0, CLASSIC_BLOCK_SIZE);
    if(page->key_a_ok[sector]) {
        memcpy(blk, page->key_a[sector], CLASSIC_KEY_SIZE);
    }
    if(acl_src) {
        memcpy(blk + 6, acl_src + 6, 4);
    } else {
        blk[6] = 0xFF;
        blk[7] = 0x07;
        blk[8] = 0x80;
        blk[9] = 0x69;
    }
    if(page->key_b_ok[sector]) {
        memcpy(blk + 10, page->key_b[sector], CLASSIC_KEY_SIZE);
    } else if(acl_src) {
        memcpy(blk + 10, acl_src + 10, CLASSIC_KEY_SIZE);
    }
    page->known[trailer] = 1;
}

static int classic_dump_sectors(ClassicDumpPage* page, volatile bool* cancel) {
    uint8_t ok_sectors = 0;
    page->sectors_ok_mask = 0;

    for(uint8_t s = 0; s < CLASSIC_1K_SECTORS; s++) {
        if(cancel && *cancel) {
            return PM3_EOPABORTED;
        }

        classic_ui(page, "Reading...", NULL, NULL);
        ClassicDumpPageModel* model = view_get_model(page->view);
        snprintf(model->line2, sizeof(model->line2), "Sector %u/16", s + 1);
        snprintf(model->line3, sizeof(model->line3), "OK %u", ok_sectors);
        view_commit_model(page->view, true);

        if(!page->key_a_ok[s] && !page->key_b_ok[s]) {
            continue;
        }

        uint8_t first = (uint8_t)(s * 4);
        uint8_t key_type = page->key_a_ok[s] ? 0 : 1;
        const uint8_t* use_key = key_type == 0 ? page->key_a[s] : page->key_b[s];

        uint8_t trailer_raw[CLASSIC_BLOCK_SIZE];
        bool got_trailer = false;
        bool sector_ok = true;

        for(uint8_t b = 0; b < 4; b++) {
            uint8_t block = (uint8_t)(first + b);
            uint8_t tmp[CLASSIC_BLOCK_SIZE];
            uint8_t use_type = key_type;
            const uint8_t* k = use_key;

            int rc = classic_read_block(block, use_type, k, tmp, cancel);
            if(rc != PM3_SUCCESS && page->key_a_ok[s] && page->key_b_ok[s]) {
                use_type = (uint8_t)(1 - key_type);
                k = (use_type == 0) ? page->key_a[s] : page->key_b[s];
                rc = classic_read_block(block, use_type, k, tmp, cancel);
            }
            if(rc != PM3_SUCCESS) {
                sector_ok = false;
                continue;
            }
            if(b == 3) {
                memcpy(trailer_raw, tmp, CLASSIC_BLOCK_SIZE);
                got_trailer = true;
            } else {
                memcpy(page->dump[block], tmp, CLASSIC_BLOCK_SIZE);
                page->known[block] = 1;
            }
        }

        if(got_trailer || page->key_a_ok[s] || page->key_b_ok[s]) {
            classic_set_trailer(page, s, got_trailer ? trailer_raw : NULL);
        }
        if(sector_ok || page->known[first]) {
            ok_sectors++;
            page->sectors_ok_mask |= (uint16_t)(1u << s);
        }
    }

    ClassicDumpPageModel* model = view_get_model(page->view);
    model->sectors_ok = ok_sectors;
    model->sectors_total = CLASSIC_1K_SECTORS;
    if(ok_sectors == 0) {
        return PM3_ESOFT;
    }
    if(ok_sectors < CLASSIC_1K_SECTORS) {
        return AUTOPWN_PARTIAL;
    }
    return PM3_SUCCESS;
}

/** Build "miss 2,5,9" (sector numbers). Fits Flipper secondary line. */
static void classic_format_missing_sectors(const ClassicDumpPage* page, char* out, size_t out_sz) {
    size_t n = 0;
    n += snprintf(out + n, out_sz - n, "miss");
    for(uint8_t s = 0; s < CLASSIC_1K_SECTORS; s++) {
        if(page->sectors_ok_mask & (uint16_t)(1u << s)) {
            continue;
        }
        if(n + 4 >= out_sz) {
            break;
        }
        n += snprintf(out + n, out_sz - n, " %u", s);
    }
    if(n == 4) { /* only "miss" */
        snprintf(out, out_sz, "miss ?");
    }
}

static uint8_t classic_missing_ab_count(const ClassicDumpPage* page) {
    uint8_t n = 0;
    for(uint8_t s = 0; s < CLASSIC_1K_SECTORS; s++) {
        if(!page->key_a_ok[s]) n++;
        if(!page->key_b_ok[s]) n++;
    }
    return n;
}

static bool classic_find_known_key(
    const ClassicDumpPage* page,
    uint8_t* out_sector,
    uint8_t* out_kt,
    const uint8_t** out_key) {
    for(uint8_t s = 0; s < CLASSIC_1K_SECTORS; s++) {
        if(page->key_a_ok[s]) {
            *out_sector = s;
            *out_kt = 0;
            *out_key = page->key_a[s];
            return true;
        }
        if(page->key_b_ok[s]) {
            *out_sector = s;
            *out_kt = 1;
            *out_key = page->key_b[s];
            return true;
        }
    }
    return false;
}

static uint32_t classic_load_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t classic_uid_cuid(const ClassicDumpPage* page) {
    if(page->uid_len >= 4) {
        return ((uint32_t)page->uid[0] << 24) | ((uint32_t)page->uid[1] << 16) |
               ((uint32_t)page->uid[2] << 8) | (uint32_t)page->uid[3];
    }
    return 0;
}

/** Append one Flipper MFKey static_nested line (parity unused by nested verify). */
static bool classic_append_nested_line(
    uint8_t sector,
    uint8_t key_type,
    uint32_t cuid,
    uint32_t nt0,
    uint32_t ks0,
    uint32_t nt1,
    uint32_t ks1) {
    char line[160];
    int n = snprintf(
        line,
        sizeof(line),
        "Sec %u key %c cuid %08lx nt0 %08lx ks0 %08lx par0 0000 nt1 %08lx ks1 %08lx par1 0000 dist 0\n",
        sector,
        key_type ? 'B' : 'A',
        (unsigned long)cuid,
        (unsigned long)nt0,
        (unsigned long)ks0,
        (unsigned long)nt1,
        (unsigned long)ks1);
    if(n <= 0 || (size_t)n >= sizeof(line)) {
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("nfc"));

    bool ok = true;
    const char* paths[2] = {NESTED_LOG_PATH, NESTED_MIRROR_PATH};
    for(uint8_t i = 0; i < 2; i++) {
        File* file = storage_file_alloc(storage);
        if(!storage_file_open(file, paths[i], FSAM_WRITE, FSOM_OPEN_APPEND)) {
            if(!storage_file_open(file, paths[i], FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                storage_file_free(file);
                ok = false;
                continue;
            }
        }
        if(storage_file_write(file, line, (size_t)n) != (size_t)n) {
            ok = false;
        }
        storage_file_close(file);
        storage_file_free(file);
    }
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static int classic_static_nested_once(
    uint8_t src_block,
    uint8_t src_kt,
    const uint8_t* key,
    uint8_t trg_block,
    uint8_t trg_kt,
    uint32_t* out_nt0,
    uint32_t* out_ks0,
    uint32_t* out_nt1,
    uint32_t* out_ks1,
    uint32_t* out_cuid,
    volatile bool* cancel) {
    struct {
        uint8_t block;
        uint8_t keytype;
        uint8_t target_block;
        uint8_t target_keytype;
        uint8_t force_detect_dist;
        uint8_t key[6];
    } PACKED payload;
    payload.block = src_block;
    payload.keytype = src_kt;
    payload.target_block = trg_block;
    payload.target_keytype = trg_kt;
    payload.force_detect_dist = 0;
    memcpy(payload.key, key, CLASSIC_KEY_SIZE);

    PacketResponseNG resp;
    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_STATIC_NESTED, (uint8_t*)&payload, sizeof(payload));
    proxmark5_com_expect_rx(STATIC_NESTED_TIMEOUT_MS);
    int rc = classic_wait(CMD_HF_MIFARE_STATIC_NESTED, &resp, STATIC_NESTED_TIMEOUT_MS, cancel);
    if(rc != PM3_SUCCESS) {
        return rc;
    }
    if(resp.status != PM3_SUCCESS || resp.length < 22) {
        return PM3_ESOFT;
    }

    const uint8_t* d = resp.data.asBytes;
    /* block, keytype, cuid[4], nt_a[4], ks_a[4], nt_b[4], ks_b[4] */
    *out_cuid = classic_load_le32(d + 2);
    *out_nt0 = classic_load_le32(d + 6);
    *out_ks0 = classic_load_le32(d + 10);
    *out_nt1 = classic_load_le32(d + 14);
    *out_ks1 = classic_load_le32(d + 18);
    return PM3_SUCCESS;
}

static int classic_collect_nonces(ClassicDumpPage* page, volatile bool* cancel) {
    page->nonces_saved = 0;
    uint8_t src_s = 0;
    uint8_t src_kt = 0;
    const uint8_t* src_key = NULL;
    if(!classic_find_known_key(page, &src_s, &src_kt, &src_key)) {
        return PM3_ESOFT;
    }

    classic_ui(page, "2/3 Nonces...", page->uid_hex, "hold card");
    int rc = classic_select_card(page, cancel);
    if(rc != PM3_SUCCESS) {
        return rc == PM3_EFAILED ? PM3_EFAILED : rc;
    }

    uint8_t src_block = (uint8_t)(src_s * 4);
    uint32_t cuid_fallback = classic_uid_cuid(page);

    for(uint8_t s = 0; s < CLASSIC_1K_SECTORS; s++) {
        for(uint8_t kt = 0; kt < 2; kt++) {
            if(cancel && *cancel) {
                return PM3_EOPABORTED;
            }
            if((kt == 0 && page->key_a_ok[s]) || (kt == 1 && page->key_b_ok[s])) {
                continue;
            }

            char prog[32];
            snprintf(prog, sizeof(prog), "S%u %c (%u)", s, kt ? 'B' : 'A', page->nonces_saved);
            classic_ui(page, "2/3 Nonces...", prog, NULL);

            uint32_t nt0 = 0, ks0 = 0, nt1 = 0, ks1 = 0, cuid = 0;
            rc = classic_static_nested_once(
                src_block,
                src_kt,
                src_key,
                (uint8_t)(s * 4),
                kt,
                &nt0,
                &ks0,
                &nt1,
                &ks1,
                &cuid,
                cancel);
            if(rc == PM3_EOPABORTED || rc == PM3_ETIMEOUT) {
                return rc;
            }
            if(rc != PM3_SUCCESS) {
                continue;
            }
            if(cuid == 0) {
                cuid = cuid_fallback;
            }
            if(classic_append_nested_line(s, kt, cuid, nt0, ks0, nt1, ks1)) {
                page->nonces_saved++;
            }
        }
    }

    return page->nonces_saved > 0 ? PM3_SUCCESS : PM3_ESOFT;
}

static int classic_run_dict(ClassicDumpPage* page, volatile bool* cancel) {
    memset(page->dump, 0, sizeof(page->dump));
    memset(page->known, 0, sizeof(page->known));
    memset(page->key_a_ok, 0, sizeof(page->key_a_ok));
    memset(page->key_b_ok, 0, sizeof(page->key_b_ok));
    page->uid_len = 0;
    page->uid_hex[0] = '\0';
    page->need_hardnested = false;
    page->sectors_ok_mask = 0;
    page->nonces_saved = 0;
    page->missing_ab = CLASSIC_1K_SECTORS * 2;

    ClassicDumpPageModel* model = view_get_model(page->view);
    model->need_hardnested = false;

    classic_ui(page, "1/3 Dict...", "Select card", "");

    int rc = classic_select_card(page, cancel);
    if(rc != PM3_SUCCESS) {
        return rc == PM3_EFAILED ? PM3_EFAILED : rc;
    }

    rc = classic_builtin_phase(page, cancel);
    if(rc == PM3_EOPABORTED || rc == PM3_ETIMEOUT) {
        return rc;
    }

    if(!classic_all_sectors_readable(page)) {
        rc = classic_dict_phase(page, cancel);
        if(rc == PM3_EOPABORTED || rc == PM3_ETIMEOUT) {
            return rc;
        }
    }

    page->missing_ab = classic_missing_ab_count(page);
    if(classic_keys_found_count(page) == 0) {
        return PM3_ESOFT;
    }
    return PM3_SUCCESS;
}

static int classic_run_dump(ClassicDumpPage* page, volatile bool* cancel) {
    memset(page->dump, 0, sizeof(page->dump));
    memset(page->known, 0, sizeof(page->known));
    page->sectors_ok_mask = 0;

    classic_ui(page, "3/3 Dump...", page->uid_hex, NULL);
    int rc = classic_select_card(page, cancel);
    if(rc != PM3_SUCCESS) {
        return rc == PM3_EFAILED ? PM3_EFAILED : rc;
    }
    return classic_dump_sectors(page, cancel);
}

static bool classic_write_nfc_file(ClassicDumpPage* page, const char* safe_name) {
    char path[96];
    snprintf(path, sizeof(path), EXT_PATH("nfc/%s.nfc"), safe_name);

    char uid_spaced[48];
    size_t pos = 0;
    for(uint8_t i = 0; i < page->uid_len && pos + 4 < sizeof(uid_spaced); i++) {
        pos += snprintf(
            uid_spaced + pos,
            sizeof(uid_spaced) - pos,
            "%s%02X",
            (i == 0) ? "" : " ",
            page->uid[i]);
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("nfc"));
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(!ok) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    char header[220];
    snprintf(
        header,
        sizeof(header),
        "Filetype: Flipper NFC device\n"
        "Version: 4\n"
        "Device type: Mifare Classic\n"
        "UID: %s\n"
        "ATQA: %02X %02X\n"
        "SAK: %02X\n"
        "Mifare Classic type: 1K\n"
        "Data format version: 2\n",
        uid_spaced,
        page->atqa_hi,
        page->atqa_lo,
        page->sak);
    ok = storage_file_write(file, header, strlen(header)) == strlen(header);

    for(uint8_t b = 0; b < CLASSIC_1K_BLOCKS && ok; b++) {
        char line[96];
        size_t lp = snprintf(line, sizeof(line), "Block %u:", b);
        for(uint8_t i = 0; i < CLASSIC_BLOCK_SIZE && lp + 4 < sizeof(line); i++) {
            if(page->known[b]) {
                lp += snprintf(line + lp, sizeof(line) - lp, " %02X", page->dump[b][i]);
            } else {
                lp += snprintf(line + lp, sizeof(line) - lp, " ??");
            }
        }
        lp += snprintf(line + lp, sizeof(line) - lp, "\n");
        ok = storage_file_write(file, line, lp) == lp;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static int32_t classic_dump_worker(void* context) {
    ClassicDumpPage* page = context;
    ClassicDumpPageModel* model = view_get_model(page->view);
    int result = PM3_ESOFT;

    if(page->worker_job == ClassicWorkerJobDict) {
        result = classic_run_dict(page, &page->worker_thread_cancel_requested);
    } else if(page->worker_job == ClassicWorkerJobCollect) {
        classic_ui(page, "2/3 Nonces...", page->uid_hex, "hold card");
        result = classic_collect_nonces(page, &page->worker_thread_cancel_requested);
    } else {
        result = classic_run_dump(page, &page->worker_thread_cancel_requested);
    }

    if(page->worker_thread_cancel_requested) {
        model->state = ClassicDumpStateIdle;
        model->can_save = false;
        model->line1[0] = model->line2[0] = model->line3[0] = '\0';
    } else if(result == PM3_EFAILED) {
        model->state = ClassicDumpStateError;
        model->can_save = false;
        snprintf(model->line1, sizeof(model->line1), "No card");
        model->line2[0] = model->line3[0] = '\0';
    } else if(result == PM3_ETIMEOUT) {
        model->state = ClassicDumpStateTimeout;
        model->can_save = false;
        snprintf(model->line1, sizeof(model->line1), "Timeout");
        model->line2[0] = model->line3[0] = '\0';
    } else if(page->worker_job == ClassicWorkerJobDict) {
        if(result == PM3_ESOFT) {
            model->state = ClassicDumpStateError;
            model->can_save = false;
            snprintf(model->line1, sizeof(model->line1), "No keys");
            snprintf(model->line2, sizeof(model->line2), "Try SD dicts");
            snprintf(model->line3, sizeof(model->line3), "or PC nested");
        } else if(result == PM3_SUCCESS) {
            model->state = ClassicDumpStateChoiceDict;
            model->can_save = false;
            snprintf(model->line1, sizeof(model->line1), "Dict done");
            snprintf(
                model->line2,
                sizeof(model->line2),
                "keys %u miss %u",
                classic_keys_found_count(page),
                page->missing_ab);
            if(page->missing_ab == 0) {
                snprintf(model->line3, sizeof(model->line3), "OK: dump");
            } else {
                snprintf(model->line3, sizeof(model->line3), "OK:nonces R:dump");
            }
        } else {
            model->state = ClassicDumpStateError;
            model->can_save = false;
            snprintf(model->line1, sizeof(model->line1), "Error %d", result);
        }
    } else if(page->worker_job == ClassicWorkerJobCollect) {
        /* Even 0 nonces: let user dump or stop */
        model->state = ClassicDumpStateChoiceNonce;
        model->can_save = false;
        if(result == PM3_SUCCESS || page->nonces_saved > 0) {
            snprintf(model->line1, sizeof(model->line1), "Nonces OK");
            snprintf(
                model->line2,
                sizeof(model->line2),
                "%u -> .nested.log",
                page->nonces_saved);
        } else {
            snprintf(model->line1, sizeof(model->line1), "No nonces");
            snprintf(model->line2, sizeof(model->line2), "card/pose?");
        }
        snprintf(model->line3, sizeof(model->line3), "OK:dump R:MFKey");
    } else if(result == AUTOPWN_PARTIAL) {
        model->state = ClassicDumpStatePartial;
        model->can_save = true;
        snprintf(model->line1, sizeof(model->line1), "Partial dump");
        snprintf(
            model->line2,
            sizeof(model->line2),
            "%u/16 %s",
            model->sectors_ok,
            page->uid_hex);
        classic_format_missing_sectors(page, model->line3, sizeof(model->line3));
    } else if(result == PM3_SUCCESS) {
        model->state = ClassicDumpStateSuccess;
        model->can_save = true;
        snprintf(model->line1, sizeof(model->line1), "Autopwn OK");
        snprintf(model->line2, sizeof(model->line2), "16/16 sectors");
        snprintf(model->line3, sizeof(model->line3), "UID %s", page->uid_hex);
    } else if(result == PM3_ESOFT) {
        model->state = ClassicDumpStateError;
        model->can_save = false;
        snprintf(model->line1, sizeof(model->line1), "Dump failed");
        snprintf(model->line2, sizeof(model->line2), "need more keys");
        model->line3[0] = '\0';
    } else {
        model->state = ClassicDumpStateError;
        model->can_save = false;
        snprintf(model->line1, sizeof(model->line1), "Error %d", result);
        model->line2[0] = model->line3[0] = '\0';
    }

    view_commit_model(page->view, true);
    classic_backlight_enforce(page, false);
    page->worker_thread_running = false;
    return 0;
}

static void classic_dump_draw(Canvas* canvas, void* context) {
    ClassicDumpPageModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 10, "MFC Autopwn");

    canvas_set_font(canvas, FontSecondary);
    switch(model->state) {
    case ClassicDumpStateIdle:
        if(model->line1[0]) {
            canvas_draw_str(canvas, 4, 22, model->line1);
            if(model->line2[0]) canvas_draw_str(canvas, 4, 32, model->line2);
            if(model->line3[0]) canvas_draw_str(canvas, 4, 42, model->line3);
        } else {
            canvas_draw_str(canvas, 4, 24, "Wizard: dict>nonce>dump");
            canvas_draw_str(canvas, 4, 36, "Choose at each step");
        }
        break;
    case ClassicDumpStateRunning:
    case ClassicDumpStateChoiceDict:
    case ClassicDumpStateChoiceNonce:
    case ClassicDumpStateSuccess:
    case ClassicDumpStatePartial:
    case ClassicDumpStateTimeout:
    case ClassicDumpStateError:
        if(model->line1[0]) canvas_draw_str(canvas, 4, 22, model->line1);
        if(model->line2[0]) canvas_draw_str(canvas, 4, 32, model->line2);
        if(model->line3[0]) canvas_draw_str(canvas, 4, 42, model->line3);
        break;
    }

    elements_button_left(canvas, "Back");
    if(model->state == ClassicDumpStateIdle) {
        elements_button_center(canvas, "Start");
    } else if(model->state == ClassicDumpStateChoiceDict) {
        elements_button_center(canvas, "Next");
        elements_button_right(canvas, "Dump");
    } else if(model->state == ClassicDumpStateChoiceNonce) {
        elements_button_center(canvas, "Dump");
        elements_button_right(canvas, "MFKey");
    } else if((model->state == ClassicDumpStateSuccess || model->state == ClassicDumpStatePartial) &&
              model->can_save) {
        elements_button_right(canvas, "Save");
    }
}

static bool classic_dump_input(InputEvent* event, void* context) {
    UNUSED(context);
    ClassicDumpPage* page = s_classic_dump_page;
    if(!page || !event || event->type != InputTypeShort) {
        return false;
    }

    ClassicDumpPageModel* model = view_get_model(page->view);
    if(event->key == InputKeyLeft) {
        classic_dump_page_reset(page);
        if(page->back_request_callback) {
            page->back_request_callback(page->back_request_context);
        }
        return true;
    }
    if(model->state == ClassicDumpStateIdle && event->key == InputKeyOk) {
        classic_dump_page_start_job(page, ClassicWorkerJobDict);
        return true;
    }
    if(model->state == ClassicDumpStateChoiceDict) {
        if(event->key == InputKeyOk) {
            if(page->missing_ab == 0) {
                classic_dump_page_start_job(page, ClassicWorkerJobDump);
            } else {
                classic_dump_page_start_job(page, ClassicWorkerJobCollect);
            }
            return true;
        }
        if(event->key == InputKeyRight) {
            classic_dump_page_start_job(page, ClassicWorkerJobDump);
            return true;
        }
    }
    if(model->state == ClassicDumpStateChoiceNonce) {
        if(event->key == InputKeyOk) {
            classic_dump_page_start_job(page, ClassicWorkerJobDump);
            return true;
        }
        if(event->key == InputKeyRight) {
            /* Exit Proxmark5 and launch MFKey FAP */
            if(page->mfkey_request_callback) {
                page->mfkey_request_callback(page->mfkey_request_context);
            } else {
                model->state = ClassicDumpStateIdle;
                model->can_save = false;
                snprintf(model->line1, sizeof(model->line1), "Nonces saved");
                snprintf(model->line2, sizeof(model->line2), "Apps>NFC>MFKey");
                snprintf(model->line3, sizeof(model->line3), "then dump again");
                view_commit_model(page->view, true);
            }
            return true;
        }
    }
    if((model->state == ClassicDumpStateSuccess || model->state == ClassicDumpStatePartial) &&
       model->can_save && (event->key == InputKeyOk || event->key == InputKeyRight)) {
        if(page->save_request_callback) {
            page->save_request_callback(page->save_request_context);
        }
        return true;
    }
    return false;
}

static void classic_dump_cleanup_worker(ClassicDumpPage* page) {
    if(page->worker_thread) {
        page->worker_thread_cancel_requested = true;
        furi_thread_join(page->worker_thread);
        furi_thread_free(page->worker_thread);
        page->worker_thread = NULL;
        page->worker_thread_running = false;
    }
    classic_backlight_enforce(page, false);
}

ClassicDumpPage* classic_dump_page_create(void) {
    ClassicDumpPage* page = calloc(1, sizeof(ClassicDumpPage));
    furi_check(page);
    page->view = view_alloc();
    page->notifications = furi_record_open(RECORD_NOTIFICATION);
    s_classic_dump_page = page;
    view_allocate_model(page->view, ViewModelTypeLockFree, sizeof(ClassicDumpPageModel));
    view_set_draw_callback(page->view, classic_dump_draw);
    view_set_input_callback(page->view, classic_dump_input);
    ClassicDumpPageModel* model = view_get_model(page->view);
    model->state = ClassicDumpStateIdle;
    view_commit_model(page->view, false);
    return page;
}

void classic_dump_page_free(ClassicDumpPage* page) {
    if(!page) {
        return;
    }
    classic_dump_page_stop(page);
    classic_backlight_enforce(page, false);
    if(page->notifications) {
        furi_record_close(RECORD_NOTIFICATION);
        page->notifications = NULL;
    }
    if(s_classic_dump_page == page) {
        s_classic_dump_page = NULL;
    }
    view_free(page->view);
    free(page);
}

View* classic_dump_page_get_view(ClassicDumpPage* page) {
    return page->view;
}

void classic_dump_page_set_save_request_callback(
    ClassicDumpPage* page,
    ClassicDumpSaveRequestCallback callback,
    void* context) {
    furi_check(page);
    page->save_request_callback = callback;
    page->save_request_context = context;
}

void classic_dump_page_set_back_request_callback(
    ClassicDumpPage* page,
    ClassicDumpBackRequestCallback callback,
    void* context) {
    furi_check(page);
    page->back_request_callback = callback;
    page->back_request_context = context;
}

void classic_dump_page_set_mfkey_request_callback(
    ClassicDumpPage* page,
    ClassicDumpMfkeyRequestCallback callback,
    void* context) {
    furi_check(page);
    page->mfkey_request_callback = callback;
    page->mfkey_request_context = context;
}

void classic_dump_page_reset(ClassicDumpPage* page) {
    if(!page) {
        return;
    }
    classic_dump_cleanup_worker(page);
    classic_backlight_enforce(page, false);
    ClassicDumpPageModel* model = view_get_model(page->view);
    model->state = ClassicDumpStateIdle;
    model->can_save = false;
    model->need_hardnested = false;
    model->sectors_ok = 0;
    model->sectors_total = CLASSIC_1K_SECTORS;
    model->line1[0] = model->line2[0] = model->line3[0] = '\0';
    view_commit_model(page->view, true);
}

const char* classic_dump_page_default_name(ClassicDumpPage* page) {
    furi_check(page);
    if(page->uid_hex[0]) {
        return page->uid_hex;
    }
    return "classic";
}

bool classic_dump_page_can_save(ClassicDumpPage* page) {
    furi_check(page);
    ClassicDumpPageModel* model = view_get_model(page->view);
    return model->can_save;
}

bool classic_dump_page_save_as(ClassicDumpPage* page, const char* name) {
    furi_check(page);
    ClassicDumpPageModel* model = view_get_model(page->view);
    char safe_name[48];
    if(!classic_sanitize_name(name, safe_name, sizeof(safe_name))) {
        snprintf(model->line3, sizeof(model->line3), "Bad name");
        view_commit_model(page->view, true);
        return false;
    }
    bool ok = classic_write_nfc_file(page, safe_name);
    if(ok) {
        /* Back to idle: ready for another run, no lingering Save button */
        model->state = ClassicDumpStateIdle;
        model->can_save = false;
        model->need_hardnested = false;
        model->sectors_ok = 0;
        model->sectors_total = CLASSIC_1K_SECTORS;
        snprintf(model->line1, sizeof(model->line1), "Saved");
        snprintf(model->line2, sizeof(model->line2), "%.40s", safe_name);
        snprintf(model->line3, sizeof(model->line3), "OK: next card");
    } else {
        snprintf(model->line3, sizeof(model->line3), "Save fail");
    }
    view_commit_model(page->view, true);
    return ok;
}

static void classic_dump_page_start_job(ClassicDumpPage* page, ClassicWorkerJob job) {
    classic_dump_cleanup_worker(page);
    page->worker_thread_cancel_requested = false;
    page->worker_job = job;
    classic_backlight_enforce(page, true);

    ClassicDumpPageModel* model = view_get_model(page->view);
    model->state = ClassicDumpStateRunning;
    model->can_save = false;
    model->need_hardnested = false;
    if(job == ClassicWorkerJobDict) {
        model->sectors_ok = 0;
        model->sectors_total = CLASSIC_1K_SECTORS;
    }
    model->line1[0] = model->line2[0] = model->line3[0] = '\0';
    view_commit_model(page->view, true);

    page->worker_thread_running = true;
    page->worker_thread = furi_thread_alloc_ex("MfcAutopwn", 6144, classic_dump_worker, page);
    if(!page->worker_thread) {
        model->state = ClassicDumpStateError;
        snprintf(model->line1, sizeof(model->line1), "Thread fail");
        view_commit_model(page->view, true);
        page->worker_thread_running = false;
        return;
    }
    furi_thread_start(page->worker_thread);
}

void classic_dump_page_start(ClassicDumpPage* page) {
    classic_dump_page_start_job(page, ClassicWorkerJobDict);
}

void classic_dump_page_stop(ClassicDumpPage* page) {
    classic_dump_page_reset(page);
}
