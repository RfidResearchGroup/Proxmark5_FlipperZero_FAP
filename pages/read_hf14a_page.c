#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include "read_hf14a_page.h"
#include "saved_tag_read.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

typedef enum {
    ReadHf14aPageStateIdle,
    ReadHf14aPageStateRunning,
    ReadHf14aPageStateReconnecting,
    ReadHf14aPageStateDisconnected,
    ReadHf14aPageStateSuccess,
    ReadHf14aPageStateNotFound,
    ReadHf14aPageStateTimeout,
    ReadHf14aPageStateError,
} ReadHf14aPageState;

#define HF14A_RECONNECT_ATTEMPTS 3

#define ATS_BYTES_PER_ROW 8
#define ATS_ROWS_PER_PAGE 3
#define ATS_BYTES_PER_PAGE (ATS_BYTES_PER_ROW * ATS_ROWS_PER_PAGE)

typedef struct {
    ReadHf14aPageState state;
    iso14a_card_select_t card;
    bool show_ats;
    uint8_t ats_page;
} ReadHf14aPageModel;

struct ReadHf14aPage {
    View* view;

    FuriThread* worker_thread;
    // The view model lock protects the worker state and pending restart.
    bool worker_restart_requested;
    volatile bool worker_thread_cancel_requested;

    ReadHf14aPageBackCallback back_callback;
    void* back_callback_context;
};

// A connect-only select is a single bounded exchange (the firmware's own
// protocol timing bounds it, not a multi-attempt loop like Hitag2's reader),
// so there is no need to send CMD_BREAK_LOOP.
static int read_hf14a_test(volatile bool* cancel_requested, iso14a_card_select_t* out_card, bool* out_found) {
    if(out_found) {
        *out_found = false;
    }
    if(out_card) {
        memset(out_card, 0, sizeof(*out_card));
    }

    iso14a_raw_cmd_t cmd = {
        .flags = ISO14A_CONNECT | ISO14A_CLEARTRACE,
        .timeout = 0,
        .wait_us = 0,
        .len = 0,
        .lenbits = 0,
    };
    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        return PM3_EMALLOC;
    }

    clearCommandBuffer();

    if(cancel_requested && *cancel_requested) {
        free(resp);
        return PM3_EOPABORTED;
    }

    if(!SendCommandNG(CMD_HF_ISO14443A_READER, (uint8_t*)&cmd, sizeof(cmd))) {
        free(resp);
        return cancel_requested && *cancel_requested ? PM3_EOPABORTED : PM3_EIO;
    }

    int wait_result = proxmark5_frame_wait(
        CMD_HF_ISO14443A_READER, resp, 2000, cancel_requested);
    if(wait_result != PM3_SUCCESS) {
        free(resp);
        return wait_result;
    }

    if(resp->status != PM3_SUCCESS ||
       resp->length < sizeof(iso14a_raw_resp_t) + sizeof(iso14a_card_select_t)) {
        free(resp);
        return PM3_ESOFT;
    }

    // resp->status is always PM3_SUCCESS here regardless of whether a card
    // was found - the real found/not-found signal is this header's sel
    // byte, and the card struct starts right after it, not at offset 0.
    const iso14a_raw_resp_t* hdr = (const iso14a_raw_resp_t*)resp->data.asBytes;
    if(hdr->sel != 0) {
        if(out_card) {
            memcpy(out_card, resp->data.asBytes + sizeof(iso14a_raw_resp_t), sizeof(*out_card));
        }
        if(out_found) {
            *out_found = true;
        }
    }

    free(resp);
    return PM3_SUCCESS;
}

static void format_uid_hex(const iso14a_card_select_t* card, char* out, size_t out_size) {
    uint8_t uidlen = card->uidlen > sizeof(card->uid) ? sizeof(card->uid) : card->uidlen;
    out[0] = '\0';
    for(uint8_t i = 0; i < uidlen && (size_t)(i * 2 + 2) < out_size; i++) {
        snprintf(&out[i * 2], out_size - (i * 2), "%02X", card->uid[i]);
    }
}

static uint8_t ats_page_count(const iso14a_card_select_t* card) {
    return (card->ats_len + ATS_BYTES_PER_PAGE - 1) / ATS_BYTES_PER_PAGE;
}

static void draw_ats(Canvas* canvas, const ReadHf14aPageModel* model) {
    const iso14a_card_select_t* card = &model->card;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 10, "ATS (incl. CRC)");
    canvas_set_font(canvas, FontSecondary);

    uint8_t pages = ats_page_count(card);
    if(pages > 1) {
        char page[10];
        snprintf(page, sizeof(page), "%u/%u", model->ats_page + 1, pages);
        canvas_draw_str_aligned(canvas, 124, 10, AlignRight, AlignBottom, page);
    }

    size_t index = model->ats_page * ATS_BYTES_PER_PAGE;
    for(uint8_t row = 0; row < ATS_ROWS_PER_PAGE && index < card->ats_len; row++) {
        char line[ATS_BYTES_PER_ROW * 3 + 1] = {0};
        for(uint8_t byte = 0; byte < ATS_BYTES_PER_ROW && index < card->ats_len; byte++) {
            snprintf(line + byte * 3, sizeof(line) - byte * 3, "%02X ",
                     card->ats[index++]);
        }
        canvas_draw_str(canvas, 4, 24 + row * 10, line);
    }

    if(model->ats_page > 0) elements_button_left(canvas, "Prev");
    elements_button_center(canvas, "Done");
    if(model->ats_page + 1 < pages) elements_button_right(canvas, "Next");
}

static void read_hf14a_page_draw_callback(Canvas* canvas, void* context) {
    ReadHf14aPageModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    if(model->show_ats) {
        draw_ats(canvas, model);
        return;
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 16, "HF 14a Reader");
    canvas_set_font(canvas, FontSecondary);

    switch(model->state) {
    case ReadHf14aPageStateIdle:
        canvas_draw_str(canvas, 4, 34, "Waiting to start...");
        break;
    case ReadHf14aPageStateRunning:
        canvas_draw_str(canvas, 4, 34, "Scanning... please wait");
        break;
    case ReadHf14aPageStateReconnecting:
        canvas_draw_str(canvas, 4, 34, "Reconnecting...");
        break;
    case ReadHf14aPageStateDisconnected:
        canvas_draw_str(canvas, 4, 34, "PM5 did not reconnect");
        break;
    case ReadHf14aPageStateSuccess: {
        char uid_hex[2 * sizeof(model->card.uid) + 1];
        format_uid_hex(&model->card, uid_hex, sizeof(uid_hex));
        char line[40];
        snprintf(line, sizeof(line), "UID: %s", uid_hex);
        bool long_uid = canvas_string_width(canvas, line) > 124;
        if(long_uid) {
            canvas_draw_str(canvas, 4, 26, "UID:");
            canvas_draw_str(canvas, 4, 37, uid_hex);
        } else {
            canvas_draw_str(canvas, 4, 30, line);
        }
        snprintf(
            line,
            sizeof(line),
            "ATQA: %02X%02X  SAK: %02X",
            model->card.atqa[1],
            model->card.atqa[0],
            model->card.sak);
        canvas_draw_str(canvas, 4, long_uid ? 49 : 42, line);
        break;
    }
    case ReadHf14aPageStateNotFound:
        canvas_draw_str(canvas, 4, 34, "No card detected");
        break;
    case ReadHf14aPageStateTimeout:
        canvas_draw_str(canvas, 4, 34, "Device not responding");
        break;
    case ReadHf14aPageStateError:
        canvas_draw_str(canvas, 4, 34, "Scan failed");
        break;
    default:
        break;
    }

    elements_button_left(canvas, model->state == ReadHf14aPageStateSuccess ? "Save" : "Back");
    if(model->state != ReadHf14aPageStateRunning &&
       model->state != ReadHf14aPageStateReconnecting) {
        elements_button_right(canvas, "Scan");
        if(model->state == ReadHf14aPageStateSuccess && model->card.ats_len >= 3) {
            elements_button_center(canvas, "ATS");
        }
    }
}

static bool read_hf14a_page_input_callback(InputEvent* event, void* context) {
    ReadHf14aPage* read_hf14a_page = context;
    if(!read_hf14a_page || !event) {
        return false;
    }

    if(event->type != InputTypeShort) {
        return false;
    }

    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    if(model->show_ats) {
        if(event->key == InputKeyOk || event->key == InputKeyBack) {
            model->show_ats = false;
        } else if(event->key == InputKeyLeft && model->ats_page > 0) {
            model->ats_page--;
        } else if(event->key == InputKeyRight &&
                  model->ats_page + 1 < ats_page_count(&model->card)) {
            model->ats_page++;
        }
        view_commit_model(read_hf14a_page->view, true);
        return true;
    }

    if(event->key == InputKeyLeft && model->state == ReadHf14aPageStateSuccess) {
        SavedTagRead tag = {.type = SavedTagHf14a, .hf14a = model->card};
        view_commit_model(read_hf14a_page->view, false);
        saved_tag_read_save(&tag);
        return true;
    }

    bool has_ats = model->state == ReadHf14aPageStateSuccess && model->card.ats_len >= 3;
    if(event->key == InputKeyOk && has_ats) {
        model->show_ats = true;
        model->ats_page = 0;
        view_commit_model(read_hf14a_page->view, true);
        return true;
    }

    bool can_scan = model->state != ReadHf14aPageStateRunning &&
                    model->state != ReadHf14aPageStateReconnecting;
    view_commit_model(read_hf14a_page->view, false);
    if(can_scan && (event->key == InputKeyRight || event->key == InputKeyOk)) {
        read_hf14a_page_start(read_hf14a_page);
        return true;
    }

    // The left button returns to Read when there is no result to save.
    // InputKeyBack is deliberately left unhandled: the ViewDispatcher routes it
    // through the view's previous-callback, which already leaves the page.
    if(event->key == InputKeyLeft) {
        read_hf14a_page_stop(read_hf14a_page);
        if(read_hf14a_page->back_callback) {
            read_hf14a_page->back_callback(read_hf14a_page->back_callback_context);
        }
        return true;
    }

    return false;
}

static void read_hf14a_page_scan(ReadHf14aPage* read_hf14a_page) {
    iso14a_card_select_t card;
    bool found = false;
    volatile bool* cancel = &read_hf14a_page->worker_thread_cancel_requested;
    int result = PM3_EOPABORTED;
    if(proxmark5_frame_begin_operation(cancel)) {
        result = read_hf14a_test(cancel, &card, &found);

        if(result == PM3_ETIMEOUT) {
            ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
            const bool reconnect = !*cancel;
            if(reconnect) {
                model->state = ReadHf14aPageStateReconnecting;
            }
            view_commit_model(read_hf14a_page->view, reconnect);

            result = PM3_ENOTTY;
            for(unsigned attempt = 0; attempt < HF14A_RECONNECT_ATTEMPTS && !*cancel; attempt++) {
                if(!proxmark5_com_handshake() || !proxmark5_frame_synchronize()) {
                    continue;
                }
                model = view_get_model(read_hf14a_page->view);
                bool retry = !*cancel;
                if(retry) {
                    model->state = ReadHf14aPageStateRunning;
                }
                view_commit_model(read_hf14a_page->view, retry);
                if(!retry) {
                    break;
                }
                result = read_hf14a_test(cancel, &card, &found);
                break;
            }
            if(*cancel) {
                result = PM3_EOPABORTED;
            }
        }

        proxmark5_frame_end_operation();
    }
    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    if(*cancel) result = PM3_EOPABORTED;
    if(result == PM3_SUCCESS) {
        if(found) {
            model->state = ReadHf14aPageStateSuccess;
            model->card = card;
        } else {
            model->state = ReadHf14aPageStateNotFound;
        }
    } else if(result == PM3_EOPABORTED) {
        model->state = ReadHf14aPageStateIdle;
    } else if(result == PM3_ENOTTY) {
        model->state = ReadHf14aPageStateDisconnected;
    } else if(result == PM3_ETIMEOUT) {
        model->state = ReadHf14aPageStateTimeout;
    } else {
        model->state = ReadHf14aPageStateError;
    }
    view_commit_model(read_hf14a_page->view, true);
}

static int32_t read_hf14a_page_worker(void* context) {
    ReadHf14aPage* page = context;
    if(page) read_hf14a_page_scan(page);
    return 0;
}

ReadHf14aPage*
    read_hf14a_page_create(ReadHf14aPageBackCallback back_callback, void* back_callback_context) {
    ReadHf14aPage* read_hf14a_page = calloc(1, sizeof(ReadHf14aPage));
    if(!read_hf14a_page) {
        return NULL;
    }

    read_hf14a_page->view = view_alloc();
    if(!read_hf14a_page->view) {
        free(read_hf14a_page);
        return NULL;
    }

    read_hf14a_page->back_callback = back_callback;
    read_hf14a_page->back_callback_context = back_callback_context;

    view_set_context(read_hf14a_page->view, read_hf14a_page);
    // Locking, not LockFree: the worker thread mutates this model while the GUI
    // thread draws from it.
    view_allocate_model(read_hf14a_page->view, ViewModelTypeLocking, sizeof(ReadHf14aPageModel));
    view_set_draw_callback(read_hf14a_page->view, read_hf14a_page_draw_callback);
    view_set_input_callback(read_hf14a_page->view, read_hf14a_page_input_callback);

    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    model->state = ReadHf14aPageStateIdle;
    memset(&model->card, 0, sizeof(model->card));
    // Required: with a locking model, view_get_model() holds the mutex until commit.
    view_commit_model(read_hf14a_page->view, false);

    return read_hf14a_page;
}

void read_hf14a_page_stop_worker(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page || !read_hf14a_page->worker_thread) return;
    read_hf14a_page_stop(read_hf14a_page);
    furi_thread_join(read_hf14a_page->worker_thread);
    furi_thread_free(read_hf14a_page->worker_thread);
    read_hf14a_page->worker_thread = NULL;
}

void read_hf14a_page_free(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page) {
        return;
    }

    read_hf14a_page_stop_worker(read_hf14a_page);
    if(read_hf14a_page->view) {
        view_free_model(read_hf14a_page->view);
        view_free(read_hf14a_page->view);
        read_hf14a_page->view = NULL;
    }

    free(read_hf14a_page);
}

View* read_hf14a_page_get_view(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page) {
        return NULL;
    }

    return read_hf14a_page->view;
}

void read_hf14a_page_tick(ReadHf14aPage* page) {
    if(!page->worker_thread || !page->worker_thread_cancel_requested ||
       furi_thread_get_state(page->worker_thread) != FuriThreadStateStopped) {
        return;
    }

    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
    if(page->worker_restart_requested) {
        read_hf14a_page_start(page);
    }
}

void read_hf14a_page_start(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page || !read_hf14a_page->view) return;

    if(read_hf14a_page->worker_thread) {
        if(furi_thread_get_state(read_hf14a_page->worker_thread) != FuriThreadStateStopped) {
            read_hf14a_page->worker_restart_requested = read_hf14a_page->worker_thread_cancel_requested;
            return;
        }
        furi_thread_free(read_hf14a_page->worker_thread);
    }

    read_hf14a_page->worker_restart_requested = false;
    read_hf14a_page->worker_thread_cancel_requested = false;
    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    model->state = ReadHf14aPageStateRunning;
    model->show_ats = false;
    model->ats_page = 0;
    memset(&model->card, 0, sizeof(model->card));
    view_commit_model(read_hf14a_page->view, true);

    read_hf14a_page->worker_thread =
        furi_thread_alloc_ex("ReadHf14aPageTask", 8192, read_hf14a_page_worker, read_hf14a_page);
    furi_thread_start(read_hf14a_page->worker_thread);
}

void read_hf14a_page_stop(ReadHf14aPage* page) {
    if(!page) return;
    page->worker_restart_requested = false;
    page->worker_thread_cancel_requested = true;
}
