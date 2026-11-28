#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/modules/text_input.h>
#include "../readers/type2_ndef.h"
#include "ntag213_write_page.h"

#define URL_PREVIEW_STEP 12U

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
    size_t url_offset;
} WritePageModel;

typedef struct {
    const Ntag21xWritePage* page;
    uint8_t row;
    uint8_t column;
    const char* notice;
} UrlInputModel;

struct Ntag21xWritePage {
    View* view;
    View* url_input;
    TextInput* input;
    FuriThread* worker;
    Ntag21xWritePageCallback on_text;
    Ntag21xWritePageCallback on_done;
    void* context;
    Ntag21xPages tag;
    Ntag21xWriteFormat format;
    size_t value_max;
    char title[16];
    char uid[2 * NTAG21X_UID_BYTES + 1];
    char text[NTAG21X_URL_MAX + 1];
};

static const char* const url_keys[] = {
    "qwertyuiop012345",
    "asdfghjkl6789.-_",
    "zxcvbnm~:/?#[]@%",
    "!$&'()*+,;=\b\r",
};

static void url_input_draw(Canvas* canvas, void* context) {
    const UrlInputModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 3, 9, model->notice ? model->notice : "URL (hold OK for caps)");
    elements_slightly_rounded_frame(canvas, 1, 12, 126, 13);
    const char* text = model->page->text;
    while(*text && canvas_string_width(canvas, text) > 115) text++;
    canvas_draw_str(canvas, 4, 22, text);
    canvas_draw_line(canvas, 4 + canvas_string_width(canvas, text), 15,
                     4 + canvas_string_width(canvas, text), 22);

    canvas_set_font(canvas, FontKeyboard);
    for(size_t row = 0; row < COUNT_OF(url_keys); row++) {
        uint8_t x = 0;
        uint8_t y = 34 + row * 9;
        for(size_t column = 0; url_keys[row][column]; column++) {
            char key = url_keys[row][column];
            bool selected = row == model->row && column == model->column;
            uint8_t width = (key == '\r' || key == '\b') ? 20 : 8;
            if(selected) {
                canvas_draw_box(canvas, x, y - 7, width, 9);
                canvas_set_color(canvas, ColorWhite);
            }
            if(key == '\b' || key == '\r') {
                canvas_draw_str(canvas, x + 2, y, key == '\b' ? "Del" : "OK");
            } else {
                canvas_draw_glyph(canvas, x + 1, y, key);
            }
            canvas_set_color(canvas, ColorBlack);
            x += width;
        }
    }
}

static bool url_input_key(InputEvent* event, void* context) {
    if(event->type != InputTypeShort &&
       (event->type != InputTypeLong || event->key != InputKeyOk) &&
       (event->type != InputTypeRepeat || event->key == InputKeyOk || event->key == InputKeyBack)) {
        return false;
    }
    Ntag21xWritePage* page = context;
    UrlInputModel* model = view_get_model(page->url_input);
    size_t width = strlen(url_keys[model->row]);
    bool accepted = false;
    bool handled = true;
    switch(event->key) {
    case InputKeyLeft:
        model->column = (model->column + width - 1) % width;
        break;
    case InputKeyRight:
        model->column = (model->column + 1) % width;
        break;
    case InputKeyUp:
        model->row = (model->row + COUNT_OF(url_keys) - 1) % COUNT_OF(url_keys);
        break;
    case InputKeyDown:
        model->row = (model->row + 1) % COUNT_OF(url_keys);
        break;
    case InputKeyOk: {
        char key = url_keys[model->row][model->column];
        size_t length = strlen(page->text);
        model->notice = NULL;
        if(key == '\r') {
            accepted = type2_ndef_url_address_valid(page->text);
            if(!accepted) model->notice = "Check the URL host/path";
        } else if(key == '\b') {
            if(length) page->text[length - 1] = '\0';
        } else if(length < page->value_max) {
            if(event->type == InputTypeLong && key >= 'a' && key <= 'z') key -= 'a' - 'A';
            page->text[length] = key;
            page->text[length + 1] = '\0';
        } else {
            model->notice = "URL is at the tag limit";
        }
        break;
    }
    default:
        handled = false;
        break;
    }
    model->column = MIN(model->column, strlen(url_keys[model->row]) - 1);
    view_commit_model(page->url_input, handled);
    if(accepted) page->on_text(page->context);
    return handled;
}

static void draw_preview(Canvas* canvas, const char* text, uint8_t y) {
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
    canvas_draw_str(canvas, 4, y, preview);
}

static void draw_url_preview(Canvas* canvas, const char* address, size_t offset) {
    if(canvas_string_width(canvas, address) <= canvas_width(canvas) - 8U) {
        canvas_draw_str(canvas, 4, 46, address);
        return;
    }

    canvas_draw_str_aligned(canvas, 124, 35, AlignRight, AlignBottom, "Up/Dn");
    if(offset) canvas_draw_str(canvas, 3, 46, "<");
    const char* part = address + offset;
    char line[NTAG21X_URL_MAX + 1];
    size_t width = 0;
    size_t length = 0;
    while(part[length]) {
        width += canvas_glyph_width(canvas, part[length]);
        if(width > canvas_width(canvas) - 21U) break;
        line[length] = part[length];
        length++;
    }
    line[length] = '\0';
    canvas_draw_str(canvas, 12, 46, line);
    if(part[length]) canvas_draw_str(canvas, 120, 46, ">");
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
        if(page->format == Ntag21xWriteText) {
            draw_preview(canvas, page->text, 35);
            canvas_draw_str(canvas, 4, 46, "Replace NDEF text?");
        } else {
            bool https = page->format == Ntag21xWriteHttps;
            canvas_draw_str(canvas, 4, 35, https ? "https://" : "http://");
            draw_url_preview(canvas, page->text, model->url_offset);
            elements_button_left(canvas, "Mode");
        }
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
    Ntag21xWriteResult result = ntag21x_write_ndef(
        &page->tag, page->format, page->text, ntag21x_write_progress, page, &cleanup_ok);
    WritePageModel* model = view_get_model(page->view);
    model->state = WritePageResult;
    model->result = result;
    model->cleanup_ok = cleanup_ok;
    view_commit_model(page->view, true);
    return 0;
}

static bool ntag21x_write_input(InputEvent* event, void* context) {
    bool scroll_key = event->key == InputKeyUp || event->key == InputKeyDown;
    if(event->type != InputTypeShort && !(event->type == InputTypeRepeat && scroll_key)) {
        return false;
    }
    Ntag21xWritePage* page = context;
    WritePageModel* model = view_get_model(page->view);
    WritePageState state = model->state;
    bool start = state == WritePageConfirm && event->key == InputKeyOk;
    bool toggle_scheme = state == WritePageConfirm && event->key == InputKeyLeft &&
                         page->format != Ntag21xWriteText;
    bool scroll = state == WritePageConfirm && scroll_key && page->format != Ntag21xWriteText;
    if(start) {
        model->state = WritePageBusy;
        model->stage = Ntag21xWriteChecking;
    } else if(toggle_scheme) {
        page->format = page->format == Ntag21xWriteHttps ? Ntag21xWriteHttp : Ntag21xWriteHttps;
    } else if(scroll && event->key == InputKeyUp && model->url_offset) {
        model->url_offset -= URL_PREVIEW_STEP;
    } else if(scroll && event->key == InputKeyDown &&
              model->url_offset + URL_PREVIEW_STEP < strlen(page->text)) {
        model->url_offset += URL_PREVIEW_STEP;
    }
    view_commit_model(page->view, start || toggle_scheme || scroll);

    if(start) {
        page->worker = furi_thread_alloc_ex("Ntag21xWrite", 4096, ntag21x_write_worker, page);
        furi_thread_start(page->worker);
        return true;
    }
    if(state == WritePageResult && (event->key == InputKeyOk || event->key == InputKeyBack)) {
        page->on_done(page->context);
        return true;
    }
    return state == WritePageBusy || toggle_scheme || scroll;
}

Ntag21xWritePage* ntag21x_write_page_alloc(
    Ntag21xWritePageCallback on_text, Ntag21xWritePageCallback on_done, void* context) {
    Ntag21xWritePage* page = calloc(1, sizeof(Ntag21xWritePage));
    page->on_text = on_text;
    page->on_done = on_done;
    page->context = context;
    page->input = text_input_alloc();
    page->url_input = view_alloc();
    view_allocate_model(page->url_input, ViewModelTypeLocking, sizeof(UrlInputModel));
    view_set_context(page->url_input, page);
    view_set_draw_callback(page->url_input, url_input_draw);
    view_set_input_callback(page->url_input, url_input_key);
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

View* ntag21x_write_page_get_url_view(Ntag21xWritePage* page) {
    return page->url_input;
}

void ntag21x_write_page_edit(
    Ntag21xWritePage* page, const Ntag21xPages* tag, Ntag21xWriteFormat format) {
    page->tag = *tag;
    page->format = format;
    page->value_max = ntag21x_write_value_max(tag, format);
    const Ntag21xModel* model = ntag21x_model_for_version(tag->version);
    snprintf(page->title, sizeof(page->title), "Write %s", model ? model->name : "NTAG21x");
    page->text[0] = '\0';
    if(format == Ntag21xWriteText) {
        text_input_reset(page->input);
        text_input_set_header_text(page->input, "NDEF text");
        text_input_set_minimum_length(page->input, 1);
        text_input_set_result_callback(
            page->input, page->on_text, page->context, page->text, page->value_max + 1, false);
    } else {
        UrlInputModel* model = view_get_model(page->url_input);
        *model = (UrlInputModel){.page = page};
        view_commit_model(page->url_input, true);
    }

    uint8_t uid[NTAG21X_UID_BYTES];
    ntag21x_pages_uid(tag, uid);
    for(size_t i = 0; i < sizeof(uid); i++) {
        snprintf(page->uid + 2 * i, 3, "%02X", uid[i]);
    }
}

void ntag21x_write_page_confirm(Ntag21xWritePage* page) {
    WritePageModel* model = view_get_model(page->view);
    model->state = WritePageConfirm;
    model->url_offset = 0;
    view_commit_model(page->view, true);
}

bool ntag21x_write_page_is_url(const Ntag21xWritePage* page) {
    return page->format != Ntag21xWriteText;
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
    view_free(page->url_input);
    text_input_free(page->input);
    free(page);
}
