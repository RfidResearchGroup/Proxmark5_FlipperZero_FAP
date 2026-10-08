#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include "read_hf14a_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

typedef enum {
    ReadHf14aPageStateIdle,
    ReadHf14aPageStateRunning,
    ReadHf14aPageStateSuccess,
    ReadHf14aPageStateNotFound,
    ReadHf14aPageStateTimeout,
    ReadHf14aPageStateError,
} ReadHf14aPageState;

typedef struct {
    ReadHf14aPageState state;
    iso14a_card_select_t card;
} ReadHf14aPageModel;

struct ReadHf14aPage {
    View* view;

    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;

    ReadHf14aPageBackCallback back_callback;
    void* back_callback_context;
};

static int read_hf14a_wait_for_response_interruptible(
    uint16_t cmd,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel_requested) {
    uint32_t start_time = furi_get_tick();

    while((furi_get_tick() - start_time) < timeout_ms) {
        if(cancel_requested && *cancel_requested) {
            return PM3_EOPABORTED;
        }

        if(proxmark5_frame_take_by_cmd(cmd, resp)) {
            return PM3_SUCCESS;
        }

        furi_delay_ms(10);
    }

    if(cancel_requested && *cancel_requested) {
        return PM3_EOPABORTED;
    }

    return proxmark5_frame_take_by_cmd(cmd, resp) ? PM3_SUCCESS : PM3_ETIMEOUT;
}

// A connect-only select is a single bounded exchange (the firmware's own
// protocol timing bounds it, not a multi-attempt loop like Hitag2's reader),
// so unlike read_hitag2_test() this never needs to send CMD_BREAK_LOOP.
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

    SendCommandNG(CMD_HF_ISO14443A_READER, (uint8_t*)&cmd, sizeof(cmd));

    int wait_result = read_hf14a_wait_for_response_interruptible(
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

static void read_hf14a_page_draw_callback(Canvas* canvas, void* context) {
    ReadHf14aPageModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
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
    case ReadHf14aPageStateSuccess: {
        char uid_hex[2 * sizeof(model->card.uid) + 1];
        format_uid_hex(&model->card, uid_hex, sizeof(uid_hex));
        char line[40];
        snprintf(line, sizeof(line), "UID: %s", uid_hex);
        canvas_draw_str(canvas, 4, 30, line);
        snprintf(
            line,
            sizeof(line),
            "ATQA: %02X%02X  SAK: %02X",
            model->card.atqa[1],
            model->card.atqa[0],
            model->card.sak);
        canvas_draw_str(canvas, 4, 42, line);
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

    elements_button_left(canvas, "Back");
}

static bool read_hf14a_page_input_callback(InputEvent* event, void* context) {
    ReadHf14aPage* read_hf14a_page = context;
    if(!read_hf14a_page || !event) {
        return false;
    }

    // The page draws a left "Back" button, so it has to actually do something.
    // InputKeyBack is deliberately left unhandled: the ViewDispatcher routes it
    // through the view's previous-callback, which already leaves the page.
    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        read_hf14a_page_stop(read_hf14a_page);
        if(read_hf14a_page->back_callback) {
            read_hf14a_page->back_callback(read_hf14a_page->back_callback_context);
        }
        return true;
    }

    return false;
}

static void read_hf14a_page_cleanup_worker(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page || !read_hf14a_page->worker_thread) {
        return;
    }

    if(read_hf14a_page->worker_thread_running) {
        return;
    }

    furi_thread_join(read_hf14a_page->worker_thread);
    furi_thread_free(read_hf14a_page->worker_thread);
    read_hf14a_page->worker_thread = NULL;
}

static int32_t read_hf14a_page_worker(void* context) {
    ReadHf14aPage* read_hf14a_page = context;
    if(!read_hf14a_page) {
        return 0;
    }

    iso14a_card_select_t card;
    bool found = false;
    int result =
        read_hf14a_test(&read_hf14a_page->worker_thread_cancel_requested, &card, &found);

    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    if(result == PM3_SUCCESS) {
        if(found) {
            model->state = ReadHf14aPageStateSuccess;
            model->card = card;
        } else {
            model->state = ReadHf14aPageStateNotFound;
        }
    } else if(result == PM3_EOPABORTED) {
        model->state = ReadHf14aPageStateIdle;
    } else if(result == PM3_ETIMEOUT) {
        model->state = ReadHf14aPageStateTimeout;
    } else {
        model->state = ReadHf14aPageStateError;
    }
    view_commit_model(read_hf14a_page->view, true);

    read_hf14a_page->worker_thread_running = false;
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

void read_hf14a_page_free(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page) {
        return;
    }

    // Runs on the app thread after the view dispatcher has stopped, so blocking
    // here is safe - same contract as read_hitag2_page_free().
    read_hf14a_page->worker_thread_cancel_requested = true;

    if(read_hf14a_page->worker_thread) {
        furi_thread_join(read_hf14a_page->worker_thread);
        furi_thread_free(read_hf14a_page->worker_thread);
        read_hf14a_page->worker_thread = NULL;
        read_hf14a_page->worker_thread_running = false;
    }

    if(read_hf14a_page->view) {
        view_set_context(read_hf14a_page->view, NULL);
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

void read_hf14a_page_start(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page || !read_hf14a_page->view) {
        return;
    }

    read_hf14a_page_cleanup_worker(read_hf14a_page);

    // Check cleanup_worker()'s pointer, not worker_thread_running - that flag
    // can flip false between the two checks and leak the old FuriThread.
    if(read_hf14a_page->worker_thread) {
        return;
    }

    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    model->state = ReadHf14aPageStateRunning;
    memset(&model->card, 0, sizeof(model->card));
    view_commit_model(read_hf14a_page->view, true);

    read_hf14a_page->worker_thread_cancel_requested = false;

    // Same stack sizing rationale as ReadHitag2Task: PacketResponseNG is
    // ~560 bytes and several live on this call path at once.
    read_hf14a_page->worker_thread =
        furi_thread_alloc_ex("ReadHf14aTask", 8192, read_hf14a_page_worker, read_hf14a_page);
    if(!read_hf14a_page->worker_thread) {
        model = view_get_model(read_hf14a_page->view);
        model->state = ReadHf14aPageStateError;
        view_commit_model(read_hf14a_page->view, true);
        return;
    }

    read_hf14a_page->worker_thread_running = true;
    furi_thread_start(read_hf14a_page->worker_thread);
}

// Called from the GUI thread (view previous-callback / left button), so it must
// never block - same non-blocking contract as read_hitag2_page_stop().
void read_hf14a_page_stop(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page || !read_hf14a_page->worker_thread) {
        return;
    }

    read_hf14a_page->worker_thread_cancel_requested = true;
}
