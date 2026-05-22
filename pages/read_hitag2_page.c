#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include "read_hitag2_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"
#include "hitag.h"

typedef enum {
    ReadHitag2PageStateIdle,
    ReadHitag2PageStateRunning,
    ReadHitag2PageStateSuccess,
    ReadHitag2PageStateTimeout,
    ReadHitag2PageStateError,
} ReadHitag2PageState;

typedef struct {
    ReadHitag2PageState state;
    int result;
    uint32_t uid;
    bool uid_valid;
} ReadHitag2PageModel;

struct ReadHitag2Page {
    View* view;

    FuriThread* worker_thread;
    bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;
};

static int read_hitag2_wait_for_response_interruptible(
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

static int
    read_hitag2_test(volatile bool* cancel_requested, uint32_t* out_uid, bool* out_uid_valid) {
    if(out_uid) {
        *out_uid = 0;
    }
    if(out_uid_valid) {
        *out_uid_valid = false;
    }

    lf_hitag_data_t* packet = malloc(sizeof(lf_hitag_data_t));
    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!packet || !resp) {
        if(packet) {
            free(packet);
        }
        if(resp) {
            free(resp);
        }
        return PM3_EMALLOC;
    }

    memset(packet, 0, sizeof(lf_hitag_data_t));

    uint8_t key[6];
    memcpy(key, "MIKR", 4);

    int pm3cmd = CMD_LF_HITAG_READER;
    packet->cmd = HT2F_PASSWORD;
    memcpy(packet->pwd, key, sizeof(packet->pwd));

    FURI_LOG_I("ReadHitag2Page", "Sending Hitag2 read command to Proxmark5");

    clearCommandBuffer();

    if(cancel_requested && *cancel_requested) {
        free(packet);
        free(resp);
        return PM3_EOPABORTED;
    }

    SendCommandNG(pm3cmd, (uint8_t*)packet, sizeof(lf_hitag_data_t));

    FURI_LOG_I("ReadHitag2Page", "Waiting for Hitag2 response from Proxmark5");

    int wait_result =
        read_hitag2_wait_for_response_interruptible(pm3cmd, resp, 2000, cancel_requested);
    if(wait_result != PM3_SUCCESS) {
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        free(packet);
        free(resp);
        if(wait_result == PM3_EOPABORTED) {
            FURI_LOG_I("ReadHitag2Page", "Hitag2 read canceled");
        } else {
            FURI_LOG_W("ReadHitag2Page", "Wait for Hitag2 response timeout");
        }
        return wait_result;
    }

    FURI_LOG_I(
        "ReadHitag2Page", "Received Hitag2 response from Proxmark5, status=%d", resp->status);

    if(resp->status != PM3_SUCCESS) {
        FURI_LOG_W("ReadHitag2Page", "Hitag2 operation failed");
        free(packet);
        free(resp);
        return PM3_ESOFT;
    }

    if(resp->length >= HITAG_UID_SIZE) {
        uint32_t uid = ((uint32_t)resp->data.asBytes[0] << 24) |
                       ((uint32_t)resp->data.asBytes[1] << 16) |
                       ((uint32_t)resp->data.asBytes[2] << 8) | (uint32_t)resp->data.asBytes[3];
        if(out_uid) {
            *out_uid = uid;
        }
        if(out_uid_valid) {
            *out_uid_valid = true;
        }
    }

    free(packet);
    free(resp);

    return PM3_SUCCESS;
}

static void read_hitag2_page_draw_callback(Canvas* canvas, void* context) {
    ReadHitag2PageModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    switch(model->state) {
    case ReadHitag2PageStateIdle:
        canvas_draw_str(canvas, 4, 16, "Read Hitag2");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 4, 34, "Waiting to start...");
        break;
    case ReadHitag2PageStateRunning:
        canvas_draw_str(canvas, 4, 16, "Read Hitag2");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 4, 34, "Reading... please wait");
        break;
    case ReadHitag2PageStateSuccess:
        canvas_draw_str(canvas, 4, 16, "Read Hitag2");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 4, 30, "Read success");
        if(model->uid_valid) {
            const char* type_str = getHitagTypeStr(model->uid);
            char uid_type_str[48] = {0x00};
            snprintf(
                uid_type_str,
                sizeof(uid_type_str),
                "UID:%08lX Type:%s",
                (unsigned long)model->uid,
                type_str ? type_str : "n/a");
            canvas_draw_str(canvas, 4, 42, uid_type_str);
        } else {
            canvas_draw_str(canvas, 4, 42, "UID:n/a Type:n/a");
        }
        break;
    case ReadHitag2PageStateTimeout:
        canvas_draw_str(canvas, 4, 16, "Read Hitag2");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 4, 34, "Read timeout");
        break;
    case ReadHitag2PageStateError:
        canvas_draw_str(canvas, 4, 16, "Read Hitag2");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 4, 34, "Read failed");
        break;
    default:
        break;
    }

    elements_button_left(canvas, "Back");
}

static void read_hitag2_page_cleanup_worker(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page || !read_hitag2_page->worker_thread) {
        return;
    }

    if(read_hitag2_page->worker_thread_running) {
        return;
    }

    furi_thread_join(read_hitag2_page->worker_thread);
    furi_thread_free(read_hitag2_page->worker_thread);
    read_hitag2_page->worker_thread = NULL;
}

static int32_t read_hitag2_page_worker(void* context) {
    ReadHitag2Page* read_hitag2_page = context;
    if(!read_hitag2_page) {
        return 0;
    }

    uint32_t uid = 0;
    bool uid_valid = false;
    int result =
        read_hitag2_test(&read_hitag2_page->worker_thread_cancel_requested, &uid, &uid_valid);

    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    model->result = result;
    if(result == PM3_SUCCESS) {
        model->state = ReadHitag2PageStateSuccess;
        model->uid = uid;
        model->uid_valid = uid_valid;
    } else if(result == PM3_EOPABORTED) {
        model->state = ReadHitag2PageStateIdle;
    } else if(result == PM3_ETIMEOUT) {
        model->state = ReadHitag2PageStateTimeout;
    } else {
        model->state = ReadHitag2PageStateError;
    }
    view_commit_model(read_hitag2_page->view, true);

    read_hitag2_page->worker_thread_running = false;
    return 0;
}

ReadHitag2Page* read_hitag2_page_create(void) {
    ReadHitag2Page* read_hitag2_page = calloc(1, sizeof(ReadHitag2Page));
    if(!read_hitag2_page) {
        return NULL;
    }

    read_hitag2_page->view = view_alloc();
    if(!read_hitag2_page->view) {
        free(read_hitag2_page);
        return NULL;
    }

    view_set_context(read_hitag2_page->view, read_hitag2_page);
    view_allocate_model(
        read_hitag2_page->view, ViewModelTypeLockFree, sizeof(ReadHitag2PageModel));
    view_set_draw_callback(read_hitag2_page->view, read_hitag2_page_draw_callback);

    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    model->state = ReadHitag2PageStateIdle;
    model->result = PM3_SUCCESS;
    model->uid = 0;
    model->uid_valid = false;

    return read_hitag2_page;
}

void read_hitag2_page_free(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page) {
        return;
    }

    read_hitag2_page->worker_thread_cancel_requested = true;
    SendCommandNG(CMD_BREAK_LOOP, NULL, 0);

    if(read_hitag2_page->worker_thread) {
        furi_thread_join(read_hitag2_page->worker_thread);
        furi_thread_free(read_hitag2_page->worker_thread);
        read_hitag2_page->worker_thread = NULL;
        read_hitag2_page->worker_thread_running = false;
    }

    if(read_hitag2_page->view) {
        view_set_context(read_hitag2_page->view, NULL);
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

void read_hitag2_page_start(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page || !read_hitag2_page->view) {
        return;
    }

    read_hitag2_page_cleanup_worker(read_hitag2_page);

    if(read_hitag2_page->worker_thread_running) {
        return;
    }

    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    model->state = ReadHitag2PageStateRunning;
    model->result = PM3_SUCCESS;
    model->uid = 0;
    model->uid_valid = false;
    view_commit_model(read_hitag2_page->view, true);

    read_hitag2_page->worker_thread_cancel_requested = false;

    // 此处的栈大小仅供测试
    read_hitag2_page->worker_thread =
        furi_thread_alloc_ex("ReadHitag2Task", 4096, read_hitag2_page_worker, read_hitag2_page);
    if(!read_hitag2_page->worker_thread) {
        model = view_get_model(read_hitag2_page->view);
        model->state = ReadHitag2PageStateError;
        model->result = PM3_EMALLOC;
        view_commit_model(read_hitag2_page->view, true);
        return;
    }

    read_hitag2_page->worker_thread_running = true;
    furi_thread_start(read_hitag2_page->worker_thread);
}

void read_hitag2_page_stop(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page || !read_hitag2_page->worker_thread) {
        return;
    }

    if(read_hitag2_page->worker_thread_running) {
        read_hitag2_page->worker_thread_cancel_requested = true;
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return;
    }

    read_hitag2_page_cleanup_worker(read_hitag2_page);
}
