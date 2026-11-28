#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/modules/text_input.h>
#include "ntag213_write_page.h"

typedef enum {
    WritePageConfirm,
    WritePageBusy,
    WritePageResult,
} WritePageState;

typedef struct {
    const Ntag21xWritePage* page;
    WritePageState state;
    Ntag21xWriteStage stage;
    Ntag21xWriteResult result;
    bool cleanup_ok;
    size_t done;
    size_t total;
} WritePageModel;

struct Ntag21xWritePage {
    View* view;
    TextInput* input;
    FuriThread* worker;
    Ntag21xWritePageCallback on_text;
    Ntag21xWritePageCallback on_done;
    void* context;
    Ntag21xPages tag;
    char title[16];
    char uid[2 * NTAG21X_UID_BYTES + 1];
    char text[NTAG21X_TEXT_MAX + 1];
};

static void draw_preview(Canvas* canvas, const char* text) {
    char preview[21];
    size_t length = strlen(text);
    if(length < sizeof(preview)) {
        memcpy(preview, text, length + 1);
    } else {
        snprintf(preview, sizeof(preview), "%.17s...", text);
        length = sizeof(preview) - 1;
    }
    size_t width = canvas_width(canvas) - 8U;
    while(length > 3 && canvas_string_width(canvas, preview) > width) {
        length--;
        memcpy(preview + (length - 3), "...", sizeof("..."));
    }
    canvas_draw_str(canvas, 4, 35, preview);
}

static void draw_result(Canvas* canvas, Ntag21xWriteResult result, bool cleanup_ok) {
    switch(result) {
    case Ntag21xWriteVerified:
        canvas_draw_str(canvas, 4, 31, cleanup_ok ? "Write verified" : "Readback matches");
        if(cleanup_ok) canvas_draw_str(canvas, 4, 43, "Readback matches");
        break;
    case Ntag21xWriteTagChanged:
        canvas_draw_str(canvas, 4, 31, "Tag missing or changed");
        canvas_draw_str(canvas, 4, 43, "Rescan before writing");
        break;
    case Ntag21xWriteProtected:
        canvas_draw_str(canvas, 4, 31, "Tag is protected");
        canvas_draw_str(canvas, 4, 43, "No pages written");
        break;
    case Ntag21xWriteUnsupported:
        canvas_draw_str(canvas, 4, 31, "Unsupported NDEF layout");
        canvas_draw_str(canvas, 4, 43, "No pages written");
        break;
    case Ntag21xWriteLinkError:
        canvas_draw_str(canvas, 4, 31, "Could not check tag");
        canvas_draw_str(canvas, 4, 43, "No pages written");
        break;
    case Ntag21xWriteNoMemory:
        canvas_draw_str(canvas, 4, 31, "Not enough memory");
        canvas_draw_str(canvas, 4, 43, "No pages written");
        break;
    case Ntag21xWriteIncomplete:
        canvas_draw_str(canvas, 4, 31, "Write not verified");
        canvas_draw_str(canvas, 4, 42, "Tag may have changed");
        break;
    }
    if(!cleanup_ok) canvas_draw_str(canvas, 4, 53, "Field off unconfirmed");
    elements_button_center(canvas, "Rescan");
}

static void ntag21x_write_draw(Canvas* canvas, void* context) {
    const WritePageModel* model = context;
    const Ntag21xWritePage* page = model->page;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, page->title);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 23, "UID");
    canvas_draw_str(canvas, 23, 23, page->uid);

    switch(model->state) {
    case WritePageConfirm:
        draw_preview(canvas, page->text);
        canvas_draw_str(canvas, 4, 46, "Replace NDEF text?");
        elements_button_center(canvas, "Write");
        break;
    case WritePageBusy:
        if(model->stage == Ntag21xWritePages) {
            char line[28];
            snprintf(line, sizeof(line), "Writing %zu/%zu pages", model->done, model->total);
            canvas_draw_str(canvas, 4, 37, line);
        } else {
            canvas_draw_str(
                canvas,
                4,
                37,
                model->stage == Ntag21xWriteVerifying ? "Verifying readback" : "Checking tag");
        }
        canvas_draw_str(canvas, 4, 52, "Keep tag in place");
        break;
    case WritePageResult:
        draw_result(canvas, model->result, model->cleanup_ok);
        break;
    }
}

static void ntag21x_write_progress(
    Ntag21xWriteStage stage, size_t done, size_t total, void* context) {
    Ntag21xWritePage* page = context;
    WritePageModel* model = view_get_model(page->view);
    model->stage = stage;
    model->done = done;
    model->total = total;
    view_commit_model(page->view, true);
}

static int32_t ntag21x_write_worker(void* context) {
    Ntag21xWritePage* page = context;
    bool cleanup_ok;
    Ntag21xWriteResult result =
        ntag21x_write_text(&page->tag, page->text, ntag21x_write_progress, page, &cleanup_ok);
    WritePageModel* model = view_get_model(page->view);
    model->state = WritePageResult;
    model->result = result;
    model->cleanup_ok = cleanup_ok;
    view_commit_model(page->view, true);
    return 0;
}

static bool ntag21x_write_input(InputEvent* event, void* context) {
    if(event->type != InputTypeShort) return false;
    Ntag21xWritePage* page = context;
    WritePageModel* model = view_get_model(page->view);
    WritePageState state = model->state;
    bool start = state == WritePageConfirm && event->key == InputKeyOk;
    if(start) {
        model->state = WritePageBusy;
        model->stage = Ntag21xWriteChecking;
    }
    view_commit_model(page->view, start);

    if(start) {
        page->worker = furi_thread_alloc_ex("Ntag21xWrite", 4096, ntag21x_write_worker, page);
        furi_thread_start(page->worker);
        return true;
    }
    if(state == WritePageResult && (event->key == InputKeyOk || event->key == InputKeyBack)) {
        page->on_done(page->context);
        return true;
    }
    return state == WritePageBusy;
}

Ntag21xWritePage* ntag21x_write_page_alloc(
    Ntag21xWritePageCallback on_text, Ntag21xWritePageCallback on_done, void* context) {
    Ntag21xWritePage* page = calloc(1, sizeof(Ntag21xWritePage));
    page->on_text = on_text;
    page->on_done = on_done;
    page->context = context;
    page->input = text_input_alloc();
    page->view = view_alloc();
    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(WritePageModel));
    WritePageModel* model = view_get_model(page->view);
    model->page = page;
    view_commit_model(page->view, false);
    view_set_input_callback(page->view, ntag21x_write_input);
    view_set_draw_callback(page->view, ntag21x_write_draw);
    return page;
}

View* ntag21x_write_page_get_view(Ntag21xWritePage* page) {
    return page->view;
}

View* ntag21x_write_page_get_input_view(Ntag21xWritePage* page) {
    return text_input_get_view(page->input);
}

void ntag21x_write_page_edit(Ntag21xWritePage* page, const Ntag21xPages* tag) {
    page->tag = *tag;
    const Ntag21xModel* model = ntag21x_model_for_version(tag->version);
    snprintf(page->title, sizeof(page->title), "Write %s", model ? model->name : "NTAG21x");
    page->text[0] = '\0';
    text_input_reset(page->input);
    text_input_set_header_text(page->input, "NDEF text");
    text_input_set_minimum_length(page->input, 1);
    text_input_set_result_callback(
        page->input,
        page->on_text,
        page->context,
        page->text,
        ntag21x_write_text_max(tag) + 1,
        false);

    uint8_t uid[NTAG21X_UID_BYTES];
    ntag21x_pages_uid(tag, uid);
    for(size_t i = 0; i < sizeof(uid); i++) {
        snprintf(page->uid + 2 * i, 3, "%02X", uid[i]);
    }
}

void ntag21x_write_page_confirm(Ntag21xWritePage* page) {
    WritePageModel* model = view_get_model(page->view);
    model->state = WritePageConfirm;
    view_commit_model(page->view, true);
}

void ntag21x_write_page_wait(Ntag21xWritePage* page) {
    if(!page->worker) return;
    furi_thread_join(page->worker);
    furi_thread_free(page->worker);
    page->worker = NULL;
}

void ntag21x_write_page_free(Ntag21xWritePage* page) {
    ntag21x_write_page_wait(page);
    view_free(page->view);
    text_input_free(page->input);
    free(page);
}
