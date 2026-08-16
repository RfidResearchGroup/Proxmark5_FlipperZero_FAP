#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
#include "simple_cmd_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

// Minimal ISO14443A flags (same bits as include/mifare.h)
#define ISO14A_CONNECT    (1U << 0)
#define ISO14A_CLEARTRACE (1U << 17)

typedef enum {
    SimpleCmdStateIdle,
    SimpleCmdStateRunning,
    SimpleCmdStateSuccess,
    SimpleCmdStateTimeout,
    SimpleCmdStateError,
} SimpleCmdState;

typedef struct {
    SimpleCmdState state;
    SimpleCmdKind kind;
    char line1[48];
    char line2[48];
    char line3[48];
    char report[768];
    uint16_t scroll_line;
    uint16_t report_lines;
    bool can_save;
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t atqa_hi;
    uint8_t atqa_lo;
    uint8_t sak;
    char uid_hex[28];
} SimpleCmdPageModel;

struct SimpleCmdPage {
    View* view;
    FuriThread* worker_thread;
    bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;
    SimpleCmdKind kind;
    SimpleCmdSaveRequestCallback save_request_callback;
    void* save_request_context;
};

static SimpleCmdPage* s_simple_cmd_page = NULL;

static const char* simple_cmd_title(SimpleCmdKind kind) {
    switch(kind) {
    case SimpleCmdPing:
        return "Ping PM5";
    case SimpleCmdVersion:
        return "HW Version";
    case SimpleCmdHf14a:
        return "HF 14a Reader";
    default:
        return "Command";
    }
}

static const char* iso14443a_card_type(uint8_t sak, uint8_t atqa_hi, uint8_t atqa_lo) {
    UNUSED(atqa_hi);
    switch(sak) {
    case 0x08:
    case 0x88:
        return "Classic 1K";
    case 0x18:
    case 0x98:
        return "Classic 4K";
    case 0x09:
        return "Classic Mini";
    case 0x00:
        if(atqa_lo == 0x44) {
            return "Ultralight";
        }
        return "UL/NTAG";
    case 0x20:
        return "ISO14443-4";
    case 0x28:
        return "Classic+ISO4";
    default:
        return "ISO14443-A";
    }
}

static int simple_cmd_wait(
    uint16_t cmd,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel_requested) {
    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < timeout_ms) {
        if(cancel_requested && *cancel_requested) {
            return PM3_EOPABORTED;
        }
        if(proxmark5_frame_take_by_cmd(cmd, resp)) {
            return PM3_SUCCESS;
        }
        furi_delay_ms(10);
    }
    return proxmark5_frame_take_by_cmd(cmd, resp) ? PM3_SUCCESS : PM3_ETIMEOUT;
}

static void simple_cmd_strip_ansi(char* text) {
    size_t read_pos = 0;
    size_t write_pos = 0;

    while(text[read_pos] != '\0') {
        unsigned char c = (unsigned char)text[read_pos];
        if(c == 0x1B && text[read_pos + 1] == '[') {
            read_pos += 2;
            while(text[read_pos] != '\0') {
                unsigned char seq = (unsigned char)text[read_pos++];
                if(seq >= 0x40 && seq <= 0x7E) {
                    break;
                }
            }
            continue;
        }
        if(c == '\n') {
            text[write_pos++] = '\n';
        } else if(c >= 0x20 && c <= 0x7E) {
            text[write_pos++] = (char)c;
        }
        read_pos++;
    }
    text[write_pos] = '\0';
}

static uint16_t simple_cmd_wrap_report(char* output, size_t output_size, char* source) {
    const size_t max_columns = 20;
    size_t out = 0;
    uint16_t lines = 0;
    char* line = source;

    while(line && *line && out + 1 < output_size) {
        char* newline = strchr(line, '\n');
        if(newline) *newline = '\0';
        while(*line == ' ') line++;

        size_t remaining = strlen(line);
        if(remaining == 0) {
            if(out + 1 < output_size) output[out++] = '\n';
            lines++;
        }

        while(remaining > 0 && out + 1 < output_size) {
            size_t chunk = remaining > max_columns ? max_columns : remaining;
            if(chunk < remaining) {
                size_t split = chunk;
                while(split > 0 && line[split] != ' ') split--;
                if(split >= 8) chunk = split;
            }
            while(chunk > 0 && line[chunk - 1] == ' ') chunk--;
            size_t room = output_size - out - 1;
            if(chunk > room) chunk = room;
            memcpy(output + out, line, chunk);
            out += chunk;
            if(out + 1 < output_size) output[out++] = '\n';
            lines++;
            line += chunk;
            while(*line == ' ') line++;
            remaining = strlen(line);
        }

        line = newline ? newline + 1 : NULL;
    }
    output[out] = '\0';
    return lines;
}

static void simple_cmd_clear_card(SimpleCmdPageModel* model) {
    model->can_save = false;
    model->uid_len = 0;
    model->atqa_hi = 0;
    model->atqa_lo = 0;
    model->sak = 0;
    model->uid_hex[0] = '\0';
    memset(model->uid, 0, sizeof(model->uid));
}

static bool simple_cmd_sanitize_name(const char* input, char* output, size_t output_size) {
    if(!input || !output || output_size < 2) {
        return false;
    }

    size_t out = 0;
    for(size_t i = 0; input[i] != '\0' && out + 1 < output_size; i++) {
        char c = input[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-') {
            output[out++] = c;
        } else if(c == ' ') {
            output[out++] = '_';
        }
    }
    output[out] = '\0';

    // Strip trailing .nfc if the user typed it (case-insensitive-ish)
    size_t len = strlen(output);
    if(len > 4) {
        const char* ext = output + len - 4;
        if((ext[0] == '.' || ext[0] == '_') &&
           (ext[1] == 'n' || ext[1] == 'N') &&
           (ext[2] == 'f' || ext[2] == 'F') &&
           (ext[3] == 'c' || ext[3] == 'C')) {
            output[len - 4] = '\0';
        }
    }

    return output[0] != '\0';
}

static bool simple_cmd_save_nfc(SimpleCmdPageModel* model, const char* name) {
    if(!model->can_save || model->uid_len == 0 || !name || name[0] == '\0') {
        return false;
    }

    char safe_name[48];
    if(!simple_cmd_sanitize_name(name, safe_name, sizeof(safe_name))) {
        return false;
    }

    char uid_spaced[48];
    size_t pos = 0;
    for(uint8_t i = 0; i < model->uid_len && pos + 4 < sizeof(uid_spaced); i++) {
        pos += snprintf(
            uid_spaced + pos,
            sizeof(uid_spaced) - pos,
            "%s%02X",
            (i == 0) ? "" : " ",
            model->uid[i]);
    }

    char content[256];
    snprintf(
        content,
        sizeof(content),
        "Filetype: Flipper NFC device\n"
        "Version: 4\n"
        "Device type: ISO14443-3A\n"
        "UID: %s\n"
        "ATQA: %02X %02X\n"
        "SAK: %02X\n",
        uid_spaced,
        model->atqa_hi,
        model->atqa_lo,
        model->sak);

    char path[96];
    snprintf(path, sizeof(path), EXT_PATH("nfc/%s.nfc"), safe_name);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("nfc"));

    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        size_t len = strlen(content);
        ok = storage_file_write(file, content, len) == len;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static int run_ping(SimpleCmdPageModel* model, volatile bool* cancel) {
    PacketResponseNG resp;
    clearCommandBuffer();
    SendCommandNG(CMD_PING, NULL, 0);
    int rc = simple_cmd_wait(CMD_PING, &resp, 4000, cancel);
    if(rc != PM3_SUCCESS) {
        return rc;
    }
    snprintf(model->line1, sizeof(model->line1), "Ping OK");
    snprintf(model->line2, sizeof(model->line2), "SPI link alive");
    model->line3[0] = '\0';
    return PM3_SUCCESS;
}

static int run_version(SimpleCmdPageModel* model, volatile bool* cancel) {
    PacketResponseNG resp;
    clearCommandBuffer();
    SendCommandNG(CMD_VERSION, NULL, 0);
    int rc = simple_cmd_wait(CMD_VERSION, &resp, 4000, cancel);
    if(rc != PM3_SUCCESS) {
        return rc;
    }
    if(resp.status != PM3_SUCCESS || resp.length < 12) {
        return PM3_ESOFT;
    }

    uint32_t chip_id = 0;
    uint32_t section_size = 0;
    uint32_t ver_len = 0;
    memcpy(&chip_id, resp.data.asBytes, 4);
    memcpy(&section_size, resp.data.asBytes + 4, 4);
    memcpy(&ver_len, resp.data.asBytes + 8, 4);

    snprintf(model->line1, sizeof(model->line1), "Chip %08lX", (unsigned long)chip_id);
    snprintf(model->line2, sizeof(model->line2), "Size %lu", (unsigned long)section_size);

    const char* mcu = "Unknown MCU";
    uint32_t flash_kb = 0;
    if(chip_id == 0x70083347U) {
        mcu = "AT32F435RGT7";
        flash_kb = 1024;
    }

    char version_text[PM3_CMD_DATA_SIZE + 1] = {0};
    if(ver_len > 1 && resp.length >= 12 + 1) {
        size_t copy = ver_len;
        if(copy > PM3_CMD_DATA_SIZE) {
            copy = PM3_CMD_DATA_SIZE;
        }
        if(12 + copy > resp.length) {
            copy = resp.length - 12;
        }
        memcpy(version_text, resp.data.asBytes + 12, copy);
        version_text[copy] = '\0';
        simple_cmd_strip_ansi(version_text);
    }

    char source[700];
    snprintf(
        source,
        sizeof(source),
        "[ Hardware ]\nMCU %s\nChip %08lX\nFlash %lu KB\nImage %lu bytes\n\n"
        "[ Firmware ]\n%s",
        mcu,
        (unsigned long)chip_id,
        (unsigned long)flash_kb,
        (unsigned long)section_size,
        version_text);
    model->report_lines =
        simple_cmd_wrap_report(model->report, sizeof(model->report), source);
    model->scroll_line = 0;
    model->line3[0] = '\0';
    return PM3_SUCCESS;
}

static int run_hf14a(SimpleCmdPageModel* model, volatile bool* cancel) {
    PacketResponseNG resp;
    simple_cmd_clear_card(model);
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT | ISO14A_CLEARTRACE, 0, 0, NULL, 0);
    int rc = simple_cmd_wait(CMD_ACK, &resp, 2500, cancel);
    if(rc != PM3_SUCCESS) {
        return rc;
    }

    // reply_mix(CMD_ACK, status, uidlen, 0, card, sizeof(card))
    if(resp.oldarg[0] == 0) {
        snprintf(model->line1, sizeof(model->line1), "No card");
        model->line2[0] = '\0';
        model->line3[0] = '\0';
        return PM3_SUCCESS;
    }

    uint8_t uidlen = (uint8_t)resp.oldarg[1];
    if(uidlen == 0 || uidlen > 10) {
        uidlen = (resp.length > 10) ? resp.data.asBytes[10] : 0;
    }
    if(uidlen == 0 || uidlen > 10 || resp.length < uidlen) {
        snprintf(model->line1, sizeof(model->line1), "Card seen");
        snprintf(model->line2, sizeof(model->line2), "Bad UID len");
        model->line3[0] = '\0';
        return PM3_SUCCESS;
    }

    memcpy(model->uid, resp.data.asBytes, uidlen);
    model->uid_len = uidlen;

    size_t pos = 0;
    for(uint8_t i = 0; i < uidlen && pos + 3 < sizeof(model->uid_hex); i++) {
        pos += snprintf(
            model->uid_hex + pos, sizeof(model->uid_hex) - pos, "%02X", model->uid[i]);
    }

    snprintf(model->line1, sizeof(model->line1), "UID %s", model->uid_hex);

    if(resp.length >= 14) {
        model->atqa_lo = resp.data.asBytes[11];
        model->atqa_hi = resp.data.asBytes[12];
        model->sak = resp.data.asBytes[13];
        model->can_save = true;

        snprintf(
            model->line2,
            sizeof(model->line2),
            "ATQA %02X%02X SAK %02X",
            model->atqa_hi,
            model->atqa_lo,
            model->sak);
        snprintf(
            model->line3,
            sizeof(model->line3),
            "%s",
            iso14443a_card_type(model->sak, model->atqa_hi, model->atqa_lo));
    } else {
        model->line2[0] = '\0';
        model->line3[0] = '\0';
    }
    return PM3_SUCCESS;
}

static int32_t simple_cmd_worker(void* context) {
    SimpleCmdPage* page = context;
    SimpleCmdPageModel* model = view_get_model(page->view);
    int result = PM3_EUNDEF;

    model->line1[0] = model->line2[0] = model->line3[0] = '\0';
    model->report[0] = '\0';
    model->scroll_line = 0;
    model->report_lines = 0;
    simple_cmd_clear_card(model);

    switch(page->kind) {
    case SimpleCmdPing:
        result = run_ping(model, &page->worker_thread_cancel_requested);
        break;
    case SimpleCmdVersion:
        result = run_version(model, &page->worker_thread_cancel_requested);
        break;
    case SimpleCmdHf14a:
        result = run_hf14a(model, &page->worker_thread_cancel_requested);
        break;
    default:
        result = PM3_EUNDEF;
        break;
    }

    if(page->worker_thread_cancel_requested) {
        model->state = SimpleCmdStateIdle;
    } else if(result == PM3_SUCCESS) {
        model->state = SimpleCmdStateSuccess;
    } else if(result == PM3_ETIMEOUT) {
        model->state = SimpleCmdStateTimeout;
        snprintf(model->line1, sizeof(model->line1), "Timeout");
    } else {
        model->state = SimpleCmdStateError;
        snprintf(model->line1, sizeof(model->line1), "Error %d", result);
    }

    view_commit_model(page->view, true);
    page->worker_thread_running = false;
    return 0;
}

static void simple_cmd_draw_report(Canvas* canvas, SimpleCmdPageModel* model) {
    const char* line = model->report;
    for(uint16_t skipped = 0; skipped < model->scroll_line && line && *line; skipped++) {
        const char* newline = strchr(line, '\n');
        line = newline ? newline + 1 : NULL;
    }

    for(uint8_t row = 0; row < 3 && line && *line; row++) {
        const char* newline = strchr(line, '\n');
        size_t len = newline ? (size_t)(newline - line) : strlen(line);
        char visible[22];
        if(len > sizeof(visible) - 1) len = sizeof(visible) - 1;
        memcpy(visible, line, len);
        visible[len] = '\0';
        canvas_draw_str(canvas, 4, 22 + (row * 10), visible);
        line = newline ? newline + 1 : NULL;
    }

    if(model->scroll_line > 0) canvas_draw_str(canvas, 121, 21, "^");
    if(model->scroll_line + 3 < model->report_lines) canvas_draw_str(canvas, 121, 42, "v");
}

static void simple_cmd_draw(Canvas* canvas, void* context) {
    SimpleCmdPageModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 10, simple_cmd_title(model->kind));

    canvas_set_font(canvas, FontSecondary);
    switch(model->state) {
    case SimpleCmdStateIdle:
        canvas_draw_str(canvas, 4, 24, "Ready");
        break;
    case SimpleCmdStateRunning:
        canvas_draw_str(canvas, 4, 24, "Running...");
        break;
    case SimpleCmdStateSuccess:
        if(model->kind == SimpleCmdVersion && model->report[0]) {
            simple_cmd_draw_report(canvas, model);
            break;
        }
        // Keep all 3 result lines above the Back button (~y 52+)
        if(model->line1[0]) canvas_draw_str(canvas, 4, 22, model->line1);
        if(model->line2[0]) canvas_draw_str(canvas, 4, 32, model->line2);
        if(model->line3[0]) canvas_draw_str(canvas, 4, 42, model->line3);
        break;
    case SimpleCmdStateTimeout:
        canvas_draw_str(canvas, 4, 24, model->line1[0] ? model->line1 : "Timeout");
        break;
    case SimpleCmdStateError:
        canvas_draw_str(canvas, 4, 24, model->line1[0] ? model->line1 : "Error");
        break;
    }

    elements_button_left(canvas, "Back");
    if(model->state == SimpleCmdStateSuccess && model->kind == SimpleCmdHf14a &&
       model->can_save) {
        elements_button_right(canvas, "Save");
    }
}

static bool simple_cmd_input(InputEvent* event, void* context) {
    UNUSED(context);
    SimpleCmdPage* page = s_simple_cmd_page;
    if(!page || !event ||
       (event->type != InputTypeShort && event->type != InputTypeRepeat)) {
        return false;
    }

    SimpleCmdPageModel* model = view_get_model(page->view);

    if(model->state == SimpleCmdStateSuccess && model->kind == SimpleCmdHf14a &&
       model->can_save && event->type == InputTypeShort &&
       (event->key == InputKeyOk || event->key == InputKeyRight)) {
        if(page->save_request_callback) {
            page->save_request_callback(page->save_request_context);
        }
        return true;
    }

    if(model->state != SimpleCmdStateSuccess || model->kind != SimpleCmdVersion) {
        return false;
    }

    uint16_t max_scroll = model->report_lines > 3 ? model->report_lines - 3 : 0;
    if(event->key == InputKeyUp && model->scroll_line > 0) {
        model->scroll_line--;
    } else if(event->key == InputKeyDown && model->scroll_line < max_scroll) {
        model->scroll_line++;
    } else {
        return false;
    }
    view_commit_model(page->view, true);
    return true;
}

static void simple_cmd_cleanup_worker(SimpleCmdPage* page) {
    if(page->worker_thread) {
        page->worker_thread_cancel_requested = true;
        furi_thread_join(page->worker_thread);
        furi_thread_free(page->worker_thread);
        page->worker_thread = NULL;
        page->worker_thread_running = false;
    }
}

SimpleCmdPage* simple_cmd_page_create(void) {
    SimpleCmdPage* page = calloc(1, sizeof(SimpleCmdPage));
    furi_check(page);
    page->view = view_alloc();
    s_simple_cmd_page = page;
    view_allocate_model(page->view, ViewModelTypeLockFree, sizeof(SimpleCmdPageModel));
    view_set_draw_callback(page->view, simple_cmd_draw);
    view_set_input_callback(page->view, simple_cmd_input);
    SimpleCmdPageModel* model = view_get_model(page->view);
    model->state = SimpleCmdStateIdle;
    model->kind = SimpleCmdPing;
    model->report[0] = '\0';
    model->scroll_line = 0;
    model->report_lines = 0;
    simple_cmd_clear_card(model);
    view_commit_model(page->view, false);
    return page;
}

void simple_cmd_page_free(SimpleCmdPage* page) {
    if(!page) {
        return;
    }
    simple_cmd_page_stop(page);
    if(s_simple_cmd_page == page) s_simple_cmd_page = NULL;
    view_free(page->view);
    free(page);
}

View* simple_cmd_page_get_view(SimpleCmdPage* page) {
    return page->view;
}

void simple_cmd_page_set_save_request_callback(
    SimpleCmdPage* page,
    SimpleCmdSaveRequestCallback callback,
    void* context) {
    furi_check(page);
    page->save_request_callback = callback;
    page->save_request_context = context;
}

const char* simple_cmd_page_default_name(SimpleCmdPage* page) {
    furi_check(page);
    SimpleCmdPageModel* model = view_get_model(page->view);
    if(model->uid_hex[0]) {
        return model->uid_hex;
    }
    return "PM5";
}

bool simple_cmd_page_save_as(SimpleCmdPage* page, const char* name) {
    furi_check(page);
    SimpleCmdPageModel* model = view_get_model(page->view);
    bool ok = simple_cmd_save_nfc(model, name);
    if(ok) {
        char safe_name[48];
        if(simple_cmd_sanitize_name(name, safe_name, sizeof(safe_name))) {
            snprintf(model->line3, sizeof(model->line3), "Saved");
            size_t base = strlen(model->line3);
            if(base + 1 < sizeof(model->line3)) {
                model->line3[base++] = ' ';
                model->line3[base] = '\0';
            }
            size_t room = sizeof(model->line3) - base - 1;
            size_t copy = strlen(safe_name);
            if(copy > room) copy = room;
            memcpy(model->line3 + base, safe_name, copy);
            model->line3[base + copy] = '\0';
        } else {
            snprintf(model->line3, sizeof(model->line3), "Saved");
        }
    } else {
        snprintf(model->line3, sizeof(model->line3), "Save fail");
    }
    view_commit_model(page->view, true);
    return ok;
}

void simple_cmd_page_set_status(SimpleCmdPage* page, const char* status) {
    furi_check(page);
    SimpleCmdPageModel* model = view_get_model(page->view);
    snprintf(model->line3, sizeof(model->line3), "%s", status ? status : "");
    view_commit_model(page->view, true);
}

void simple_cmd_page_start(SimpleCmdPage* page, SimpleCmdKind kind) {
    simple_cmd_cleanup_worker(page);
    page->kind = kind;
    page->worker_thread_cancel_requested = false;

    SimpleCmdPageModel* model = view_get_model(page->view);
    model->kind = kind;
    model->state = SimpleCmdStateRunning;
    model->line1[0] = model->line2[0] = model->line3[0] = '\0';
    model->report[0] = '\0';
    model->scroll_line = 0;
    model->report_lines = 0;
    simple_cmd_clear_card(model);
    view_commit_model(page->view, true);

    page->worker_thread_running = true;
    page->worker_thread = furi_thread_alloc_ex("SimpleCmd", 4096, simple_cmd_worker, page);
    if(!page->worker_thread) {
        model->state = SimpleCmdStateError;
        snprintf(model->line1, sizeof(model->line1), "Thread fail");
        view_commit_model(page->view, true);
        page->worker_thread_running = false;
        return;
    }
    furi_thread_start(page->worker_thread);
}

void simple_cmd_page_stop(SimpleCmdPage* page) {
    if(!page) {
        return;
    }
    // Do not start a second synchronous SPI command while the worker owns the
    // bus; that deadlocked Back/exit and forced a full Flipper reboot.
    simple_cmd_cleanup_worker(page);
}
