#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include "read_hitag2_page.h"
#include "saved_tag_read.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"
#include "hitag.h"

#define HITAG2_RECONNECT_ATTEMPTS 3
#define HITAG2_UID_ATTEMPTS 4

typedef enum {
    ReadHitag2PageStateIdle,
    ReadHitag2PageStateRunning,
    ReadHitag2PageStateReconnecting,
    ReadHitag2PageStateDisconnected,
    ReadHitag2PageStateSuccess,
    ReadHitag2PageStateTimeout,
    ReadHitag2PageStateError,
} ReadHitag2PageState;

typedef struct {
    ReadHitag2PageState state;
    int result;
    uint32_t uid;
} ReadHitag2PageModel;

struct ReadHitag2Page {
    View* view;

    FuriThread* worker_thread;
    // The view model lock protects the worker state and pending restart.
    bool worker_restart_requested;
    volatile bool worker_thread_cancel_requested;

    ReadHitag2PageBackCallback back_callback;
    void* back_callback_context;
};

static const char* getHitagTypeStr(uint32_t uid) {
    //uid s/n        ********
    uint8_t type = (uid >> 4) & 0xF;
    switch(type) {
    case 1:
        return "PCF 7936";
    case 2:
        return "PCF 7946";
    case 3:
        return "PCF 7947";
    case 4:
        return "PCF 7942/44";
    case 5:
        return "PCF 7943";
    case 6:
        return "PCF 7941";
    case 7:
        return "PCF 7952";
    case 8:
        return "PCF 7961";
    case 9:
        return "PCF 7945";
    default:
        return "";
    }
}

static int read_hitag2_uid(volatile bool* cancel_requested, uint32_t* uid) {
    if(cancel_requested && *cancel_requested) {
        return PM3_EOPABORTED;
    }

    lf_hitag_data_t packet = {.cmd = HT2F_UID_ONLY};
    PacketResponseNG response;
    clearCommandBuffer();
    if(!SendCommandNG(CMD_LF_HITAG_READER, (uint8_t*)&packet, sizeof(packet))) {
        return cancel_requested && *cancel_requested ? PM3_EOPABORTED : PM3_EIO;
    }

    int result = proxmark5_frame_wait(
        CMD_LF_HITAG_READER, &response, 2000, cancel_requested);
    if(result != PM3_SUCCESS) {
        // A UID-only request ends on its own.
        return result;
    }
    if(response.status != PM3_SUCCESS) {
        return response.status;
    }
    if(response.length != HITAG_UID_SIZE) {
        return PM3_ESOFT;
    }

    *uid = ((uint32_t)response.data.asBytes[0] << 24) |
           ((uint32_t)response.data.asBytes[1] << 16) |
           ((uint32_t)response.data.asBytes[2] << 8) | (uint32_t)response.data.asBytes[3];
    return PM3_SUCCESS;
}

static int read_hitag2_scan(volatile bool* cancel, uint32_t* out_uid) {
    uint32_t candidate_uid = 0;
    bool have_uid = false;

    // A single UID reply has no checksum from the tag.
    for(unsigned attempt = 0; attempt < HITAG2_UID_ATTEMPTS; attempt++) {
        uint32_t uid;
        int result = read_hitag2_uid(cancel, &uid);
        if(result == PM3_EFAILED) {
            continue;
        }
        if(result != PM3_SUCCESS) {
            return result;
        }
        if(have_uid) {
            if(uid != candidate_uid) {
                return PM3_EWRONGANSWER;
            }
            *out_uid = uid;
            return PM3_SUCCESS;
        }
        candidate_uid = uid;
        have_uid = true;
    }

    return have_uid ? PM3_EWRONGANSWER : PM3_EFAILED;
}

static void read_hitag2_page_draw_callback(Canvas* canvas, void* context) {
    ReadHitag2PageModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 13, "Hitag2 Reader");
    canvas_set_font(canvas, FontSecondary);

    switch(model->state) {
    case ReadHitag2PageStateIdle:
        canvas_draw_str(canvas, 4, 34, "Waiting to start...");
        break;
    case ReadHitag2PageStateRunning:
        canvas_draw_str(canvas, 4, 34, "Reading... please wait");
        break;
    case ReadHitag2PageStateReconnecting:
        canvas_draw_str(canvas, 4, 34, "Reconnecting to PM5...");
        break;
    case ReadHitag2PageStateDisconnected:
        canvas_draw_str(canvas, 4, 34, "PM5 did not reconnect");
        break;
    case ReadHitag2PageStateSuccess: {
        char line[32];
        const char* type = getHitagTypeStr(model->uid);
        canvas_draw_str(canvas, 4, 26, "Read success");
        snprintf(line, sizeof(line), "UID: %08lX", (unsigned long)model->uid);
        canvas_draw_str(canvas, 4, 38, line);
        snprintf(line, sizeof(line), "Type: %s", type[0] ? type : "Unknown");
        canvas_draw_str(canvas, 4, 50, line);
        break;
    }
    case ReadHitag2PageStateTimeout:
        canvas_draw_str(canvas, 4, 34, "Read timeout");
        break;
    case ReadHitag2PageStateError:
        if(model->result == PM3_EFAILED) {
            canvas_draw_str(canvas, 4, 28, "No Hitag2 UID read");
            canvas_draw_str(canvas, 4, 41, "Move tag closer");
        } else if(model->result == PM3_EWRONGANSWER) {
            canvas_draw_str(canvas, 4, 28, "Could not confirm UID");
            canvas_draw_str(canvas, 4, 41, "Hold tag still");
        } else if(model->result == PM3_EIO) {
            canvas_draw_str(canvas, 4, 34, "Send failed. Check link.");
        } else if(model->result == PM3_ENOTIMPL) {
            canvas_draw_str(canvas, 4, 34, "Reader unavailable");
        } else {
            canvas_draw_str(canvas, 4, 34, "Read failed");
        }
        break;
    default:
        break;
    }

    elements_button_left(canvas, model->state == ReadHitag2PageStateSuccess ? "Save" : "Back");
    if(model->state != ReadHitag2PageStateRunning &&
       model->state != ReadHitag2PageStateReconnecting) {
        elements_button_right(canvas, "Scan Again");
    }
}

static bool read_hitag2_page_input_callback(InputEvent* event, void* context) {
    ReadHitag2Page* read_hitag2_page = context;
    if(!read_hitag2_page || !event) {
        return false;
    }

    if(event->type != InputTypeShort) {
        return false;
    }

    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    if(event->key == InputKeyLeft && model->state == ReadHitag2PageStateSuccess) {
        SavedTagRead tag = {.type = SavedTagHitag2, .hitag2_uid = model->uid};
        view_commit_model(read_hitag2_page->view, false);
        saved_tag_read_save(&tag);
        return true;
    }

    ReadHitag2PageState state = model->state;
    view_commit_model(read_hitag2_page->view, false);

    if(event->key == InputKeyRight || event->key == InputKeyOk) {
        if(state != ReadHitag2PageStateRunning && state != ReadHitag2PageStateReconnecting) {
            read_hitag2_page_start(read_hitag2_page);
        }
        return true;
    }

    // The left button returns to Read when there is no result to save.
    // The ViewDispatcher routes InputKeyBack through the view's previous-callback.
    if(event->key == InputKeyLeft) {
        read_hitag2_page_stop(read_hitag2_page);
        if(read_hitag2_page->back_callback) {
            read_hitag2_page->back_callback(read_hitag2_page->back_callback_context);
        }
        return true;
    }

    return false;
}

static int read_hitag2_reconnect(volatile bool* cancel, uint32_t* uid) {
    for(unsigned attempt = 0; attempt < HITAG2_RECONNECT_ATTEMPTS && !*cancel; attempt++) {
        if(proxmark5_com_handshake()) {
            if(!proxmark5_frame_synchronize()) {
                break;
            }
            int result = read_hitag2_scan(cancel, uid);
            if(result == PM3_ETIMEOUT && proxmark5_frame_synchronize()) {
                result = read_hitag2_scan(cancel, uid);
            }
            return result;
        }
    }
    return *cancel ? PM3_EOPABORTED : PM3_ENOTTY;
}

static void read_hitag2_page_scan(ReadHitag2Page* read_hitag2_page) {
    uint32_t uid = 0;
    volatile bool* cancel = &read_hitag2_page->worker_thread_cancel_requested;
    int result = PM3_EOPABORTED;
    if(proxmark5_frame_begin_operation(cancel)) {
        result = read_hitag2_scan(cancel, &uid);
        if(result == PM3_ETIMEOUT) {
            ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
            const bool reconnect = !*cancel;
            if(reconnect) {
                model->state = ReadHitag2PageStateReconnecting;
            }
            view_commit_model(read_hitag2_page->view, reconnect);

            result = reconnect ? read_hitag2_reconnect(cancel, &uid) : PM3_EOPABORTED;
        }

        proxmark5_frame_end_operation();
    }
    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    if(*cancel) result = PM3_EOPABORTED;
    model->result = result;
    if(result == PM3_SUCCESS) {
        model->state = ReadHitag2PageStateSuccess;
        model->uid = uid;
    } else if(result == PM3_EOPABORTED) {
        model->state = ReadHitag2PageStateIdle;
    } else if(result == PM3_ENOTTY) {
        model->state = ReadHitag2PageStateDisconnected;
    } else if(result == PM3_ETIMEOUT) {
        model->state = ReadHitag2PageStateTimeout;
    } else {
        model->state = ReadHitag2PageStateError;
    }
    view_commit_model(read_hitag2_page->view, true);
}

static int32_t read_hitag2_page_worker(void* context) {
    ReadHitag2Page* page = context;
    if(page) read_hitag2_page_scan(page);
    return 0;
}

ReadHitag2Page*
    read_hitag2_page_create(ReadHitag2PageBackCallback back_callback, void* back_callback_context) {
    ReadHitag2Page* read_hitag2_page = calloc(1, sizeof(ReadHitag2Page));
    if(!read_hitag2_page) {
        return NULL;
    }

    read_hitag2_page->view = view_alloc();
    if(!read_hitag2_page->view) {
        free(read_hitag2_page);
        return NULL;
    }

    read_hitag2_page->back_callback = back_callback;
    read_hitag2_page->back_callback_context = back_callback_context;

    view_set_context(read_hitag2_page->view, read_hitag2_page);
    // Locking, not LockFree: the worker thread mutates this model while the GUI
    // thread draws from it.
    view_allocate_model(read_hitag2_page->view, ViewModelTypeLocking, sizeof(ReadHitag2PageModel));
    view_set_draw_callback(read_hitag2_page->view, read_hitag2_page_draw_callback);
    view_set_input_callback(read_hitag2_page->view, read_hitag2_page_input_callback);

    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    model->state = ReadHitag2PageStateIdle;
    model->result = PM3_SUCCESS;
    model->uid = 0;
    // Required: with a locking model, view_get_model() holds the mutex until commit.
    view_commit_model(read_hitag2_page->view, false);

    return read_hitag2_page;
}

void read_hitag2_page_stop_worker(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page || !read_hitag2_page->worker_thread) return;
    read_hitag2_page_stop(read_hitag2_page);
    furi_thread_join(read_hitag2_page->worker_thread);
    furi_thread_free(read_hitag2_page->worker_thread);
    read_hitag2_page->worker_thread = NULL;
}

void read_hitag2_page_free(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page) {
        return;
    }

    read_hitag2_page_stop_worker(read_hitag2_page);
    if(read_hitag2_page->view) {
        view_free_model(read_hitag2_page->view);
        view_free(read_hitag2_page->view);
        read_hitag2_page->view = NULL;
    }

    free(read_hitag2_page);
}

View* read_hitag2_page_get_view(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page) {
        return NULL;
    }

    return read_hitag2_page->view;
}

void read_hitag2_page_tick(ReadHitag2Page* page) {
    if(!page->worker_thread || !page->worker_thread_cancel_requested ||
       furi_thread_get_state(page->worker_thread) != FuriThreadStateStopped) {
        return;
    }

    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
    if(page->worker_restart_requested) {
        read_hitag2_page_start(page);
    }
}

void read_hitag2_page_start(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page || !read_hitag2_page->view) return;

    if(read_hitag2_page->worker_thread) {
        if(furi_thread_get_state(read_hitag2_page->worker_thread) != FuriThreadStateStopped) {
            read_hitag2_page->worker_restart_requested = read_hitag2_page->worker_thread_cancel_requested;
            return;
        }
        furi_thread_free(read_hitag2_page->worker_thread);
    }

    read_hitag2_page->worker_restart_requested = false;
    read_hitag2_page->worker_thread_cancel_requested = false;
    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    model->state = ReadHitag2PageStateRunning;
    model->result = PM3_SUCCESS;
    model->uid = 0;
    view_commit_model(read_hitag2_page->view, true);

    read_hitag2_page->worker_thread =
        furi_thread_alloc_ex("ReadHitag2PageTask", 8192, read_hitag2_page_worker, read_hitag2_page);
    furi_thread_start(read_hitag2_page->worker_thread);
}

void read_hitag2_page_stop(ReadHitag2Page* page) {
    if(!page) return;
    page->worker_restart_requested = false;
    page->worker_thread_cancel_requested = true;
}
