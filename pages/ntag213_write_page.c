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
    uint8_t review_field;
} WritePageModel;

typedef struct {
    const Ntag21xWritePage* page;
    uint8_t row;
    uint8_t column;
    const char* notice;
    bool symbols;
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
    uint8_t field;
    bool https;
    char title[16];
    char uid[2 * NTAG21X_UID_BYTES + 1];
    char fields[3][NTAG21X_URL_MAX + 1];
};

static const char* const url_keys[] = {
    "qwertyuiop012345",
    "asdfghjkl6789.-_",
    "zxcvbnm~:/?#[]@%",
    "!$&'()*+,;=\b\r",
};

static const char* const text_keys[] = {
    "qwertyuiop012345",
    "asdfghjkl6789.-_",
    "zxcvbnm~:/?#[]@%",
    "!$&*+,\t \b\r",
};

static const char* const symbol_keys[] = {
    "\"\\<>^`{|}~:/?#[]",
    "!$&'()*+,;=%@_-",
    "0123456789.",
    "\t \b\r",
};

static bool link_pair(const Ntag21xWritePage* page) {
    return page->format == Ntag21xWriteTextAndUrl || page->format == Ntag21xWriteSmartPoster;
}

static unsigned field_count(const Ntag21xWritePage* page) {
    if(page->format == Ntag21xWriteVCard) return 3;
    return link_pair(page) || page->format == Ntag21xWriteWifi ? 2 : 1;
}

static bool field_is_key(const Ntag21xWritePage* page) {
    return page->format == Ntag21xWriteWifi && page->field == 1;
}

static size_t field_limit(const Ntag21xWritePage* page) {
    if(page->format == Ntag21xWriteWifi) {
        return field_is_key(page) ? TYPE2_NDEF_WIFI_KEY_MAX : TYPE2_NDEF_WIFI_SSID_MAX;
    }
    return page->value_max;
}

static bool field_is_url(const Ntag21xWritePage* page) {
    return page->format == Ntag21xWriteUrl ||
           (link_pair(page) && page->field == 1);
}

static const char* const* editor_keys(const UrlInputModel* model) {
    if(field_is_url(model->page)) return url_keys;
    return model->symbols ? symbol_keys : text_keys;
}

static const char* field_label(const Ntag21xWritePage* page) {
    if(field_is_url(page)) return "URL (hold OK for caps)";
    if(page->format == Ntag21xWriteWifi) {
        return field_is_key(page) ? "Password (blank = open)" : "SSID (hold OK for caps)";
    }
    if(page->format == Ntag21xWriteVCard) {
        static const char* const labels[] = {
            "Name (hold OK for caps)", "Phone (optional)", "Email (optional)",
        };
        return labels[page->field];
    }
    return page->format == Ntag21xWriteSmartPoster ? "Title (hold OK for caps)" :
                                                    "Text (hold OK for caps)";
}

static const char* validate_field(const Ntag21xWritePage* page) {
    const char* value = page->fields[page->field];
    if(page->format == Ntag21xWriteWifi) {
        if(field_is_key(page)) return type2_ndef_wifi_key_valid(value) ? NULL : "Use 8-63 chars or hex";
        return *value ? NULL : "Enter a network name";
    }
    if(field_is_url(page)) {
        return type2_ndef_url_address_valid(value) ? NULL : "Check the URL host/path";
    }
    if(page->format == Ntag21xWriteVCard) {
        return page->field || *value ? NULL : "Enter a name";
    }
    return *value ? NULL : "Enter text";
}

static void field_values(const Ntag21xWritePage* page, const char* values[3]) {
    for(size_t i = 0; i < COUNT_OF(page->fields); i++) values[i] = page->fields[i];
}

static void url_input_draw(Canvas* canvas, void* context) {
    const UrlInputModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    const Ntag21xWritePage* page = model->page;
    const char* const* keys = editor_keys(model);
    canvas_draw_str(canvas, 3, 9, model->notice ? model->notice : field_label(page));
    elements_slightly_rounded_frame(canvas, 1, 12, 126, 13);
    const char* text = page->fields[page->field];
    while(*text && canvas_string_width(canvas, text) > 115) text++;
    canvas_draw_str(canvas, 4, 22, text);
    canvas_draw_line(canvas, 4 + canvas_string_width(canvas, text), 15,
                     4 + canvas_string_width(canvas, text), 22);

    canvas_set_font(canvas, FontKeyboard);
    for(size_t row = 0; row < COUNT_OF(url_keys); row++) {
        uint8_t x = 0;
        uint8_t y = 34 + row * 9;
        for(size_t column = 0; keys[row][column]; column++) {
            char key = keys[row][column];
            bool selected = row == model->row && column == model->column;
            uint8_t width = (key == '\r' || key == '\b' || key == ' ' || key == '\t') ? 20 : 8;
            if(selected) {
                canvas_draw_box(canvas, x, y - 7, width, 9);
                canvas_set_color(canvas, ColorWhite);
            }
            if(width == 20) {
                const char* label = key == '\t' ? (model->symbols ? "ABC" : "Sym") :
                                    key == '\b' ? "Del" : (key == '\r' ? "OK" : "Sp");
                canvas_draw_str(canvas, x + 2, y, label);
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
    const char* const* keys = editor_keys(model);
    size_t width = strlen(keys[model->row]);
    char* value = page->fields[page->field];
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
        char key = keys[model->row][model->column];
        size_t length = strlen(value);
        model->notice = NULL;
        if(key == '\r') {
            model->notice = validate_field(page);
            if(model->notice) break;
            if(page->field + 1U < field_count(page)) {
                page->field++;
                model->symbols = false;
                model->row = 0;
                model->column = 0;
            } else {
                const char* values[3];
                field_values(page, values);
                accepted = ntag21x_write_can_encode(&page->tag, page->format, values, page->https);
                if(!accepted) model->notice = "Record too big for tag";
            }
        } else if(key == '\t') {
            model->symbols = !model->symbols;
            model->column = model->symbols ? 0 : strlen(text_keys[model->row]) - 4;
        } else if(key == '\b') {
            if(length) value[length - 1] = '\0';
        } else if(length < field_limit(page)) {
            if(event->type == InputTypeLong && key >= 'a' && key <= 'z') key -= 'a' - 'A';
            value[length] = key;
            value[length + 1] = '\0';
        } else {
            model->notice = "Field is at its limit";
        }
        break;
    }
    case InputKeyBack:
        if(page->field) {
            page->field--;
            model->symbols = false;
            model->row = 0;
            model->column = 0;
            model->notice = NULL;
        } else {
            if(page->format == Ntag21xWriteWifi) memset(page->fields[1], 0, sizeof(page->fields[1]));
            handled = false;
        }
        break;
    default:
        handled = false;
        break;
    }
    keys = editor_keys(model);
    model->column = MIN(model->column, strlen(keys[model->row]) - 1);
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
            draw_preview(canvas, page->fields[0], 35);
            canvas_draw_str(canvas, 4, 46, "Replace NDEF text?");
        } else {
            const char* scheme = page->https ? "https://" : "http://";
            if(page->format == Ntag21xWriteUrl) {
                canvas_draw_str(canvas, 4, 35, scheme);
                draw_url_preview(canvas, page->fields[0], model->url_offset);
            } else if(link_pair(page)) {
                const char* label = page->format == Ntag21xWriteSmartPoster ? "Title" : "Text";
                canvas_draw_str(canvas, 4, 35, model->review_field ? scheme : label);
                draw_url_preview(canvas, page->fields[model->review_field], model->url_offset);
                elements_button_right(canvas, "Next");
            } else if(page->format == Ntag21xWriteWifi) {
                canvas_draw_str(canvas, 4, 35, *page->fields[1] ? "WPA2 Personal" : "Open network");
                draw_url_preview(canvas, page->fields[0], model->url_offset);
            } else {
                static const char* const labels[] = {"Name", "Phone", "Email"};
                const char* value = page->fields[model->review_field];
                canvas_draw_str(canvas, 4, 35, labels[model->review_field]);
                draw_url_preview(canvas, *value ? value : "(blank)", model->url_offset);
                elements_button_right(canvas, "Next");
            }
            if(page->format == Ntag21xWriteUrl || link_pair(page)) {
                elements_button_left(canvas, "Mode");
            }
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
    const char* values[3];
    field_values(page, values);
    Ntag21xWriteResult result = ntag21x_write_ndef(
        &page->tag, page->format, values, page->https, ntag21x_write_progress, page, &cleanup_ok);
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
                         (page->format == Ntag21xWriteUrl || link_pair(page));
    bool next = state == WritePageConfirm && event->key == InputKeyRight &&
                page->format != Ntag21xWriteWifi && field_count(page) > 1;
    bool scroll = state == WritePageConfirm && scroll_key && page->format != Ntag21xWriteText;
    if(start) {
        model->state = WritePageBusy;
        model->stage = Ntag21xWriteChecking;
    } else if(toggle_scheme) {
        page->https = !page->https;
        if(link_pair(page)) {
            model->review_field = 1;
            model->url_offset = 0;
        }
    } else if(next) {
        model->review_field = (model->review_field + 1) % field_count(page);
        model->url_offset = 0;
    } else if(scroll && event->key == InputKeyUp && model->url_offset) {
        model->url_offset -= URL_PREVIEW_STEP;
    } else if(scroll && event->key == InputKeyDown &&
              model->url_offset + URL_PREVIEW_STEP < strlen(page->fields[model->review_field])) {
        model->url_offset += URL_PREVIEW_STEP;
    }
    view_commit_model(page->view, start || toggle_scheme || next || scroll);

    if(start) {
        page->worker = furi_thread_alloc_ex("Ntag21xWrite", 4096, ntag21x_write_worker, page);
        furi_thread_start(page->worker);
        return true;
    }
    if(state == WritePageResult && (event->key == InputKeyOk || event->key == InputKeyBack)) {
        page->on_done(page->context);
        return true;
    }
    return state == WritePageBusy || toggle_scheme || next || scroll;
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
    page->field = 0;
    page->https = true;
    memset(page->fields, 0, sizeof(page->fields));
    if(format == Ntag21xWriteText) {
        text_input_reset(page->input);
        text_input_set_header_text(page->input, "NDEF text");
        text_input_set_minimum_length(page->input, 1);
        text_input_set_result_callback(
            page->input, page->on_text, page->context, page->fields[0], page->value_max + 1, false);
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
    model->review_field = 0;
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
    if(page->format == Ntag21xWriteWifi) memset(page->fields[1], 0, sizeof(page->fields[1]));
}

void ntag21x_write_page_free(Ntag21xWritePage* page) {
    ntag21x_write_page_wait(page);
    view_free(page->view);
    view_free(page->url_input);
    text_input_free(page->input);
    free(page);
}
