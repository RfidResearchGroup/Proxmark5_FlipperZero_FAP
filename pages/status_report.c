#include <furi.h>
#include <stdarg.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include "proxmark5_frame.h"
#include "../readers/lf_capture.h"
#include "status_report.h"

#define REPORT_CAPACITY 8192
#define REPORT_WIDTH 118
#define REPORT_ROWS 5
#define REPORT_HEADING '\x01'

typedef struct {
    const char* message;
    char* text;
    size_t rows;
    size_t top;
} StatusReportModel;

struct StatusReport {
    View* view;
    FuriThread* worker;
    volatile bool cancel;
    bool pending;
};

typedef struct {
    char* text;
    size_t length;
    bool truncated;
} ReportText;

static void report_append(ReportText* report, const char* format, ...) {
    const size_t limit = REPORT_CAPACITY - sizeof("\n[Report truncated]");
    if(report->truncated) return;

    va_list args;
    va_start(args, format);
    int length = vsnprintf(report->text + report->length, limit - report->length, format, args);
    va_end(args);

    if(length < 0 || (size_t)length >= limit - report->length) {
        report->text[report->length] = '\0';
        report->truncated = true;
    } else {
        report->length += length;
    }
}

static char* status_report_normalize(char* line) {
    while(*line == ' ' || *line == '\t') line++;
    char* out = line;
    bool space = false;
    for(const char* in = line; *in;) {
        if(*in == ' ' || *in == '\t' || *in == '\r') {
            space = out != line;
            in++;
            continue;
        }
        if(out != line && in[0] == '.' && in[1] == '.') {
            const char* end = in + 2;
            while(*end == '.') end++;
            if(*end == ' ' || *end == '\t') {
                *out++ = ':';
                in = end;
                space = false;
                continue;
            }
        }
        if(space && *in != ')' && out[-1] != '(') *out++ = ' ';
        *out++ = *in++;
        space = false;
    }
    *out = '\0';
    return line;
}

static void status_report_line(ReportText* text, const char* line, bool heading) {
    static const struct {
        const char* source;
        const char* label;
    } fields[] = {
        {"BigBuf_size", "BigBuf size"},
        {"tracing", "Enabled"},
        {"traceLen", "Trace length"},
        {"mode", "Mode"},
        {"usercode", "User code"},
        {"ToSendMax", "ToSend max"},
        {"ToSend BUFFERSIZE", "ToSend buffer"},
    };
    if(heading) report_append(text, "%c", REPORT_HEADING);
    const char* colon = strchr(line, ':');
    if(colon) {
        size_t length = colon - line;
        for(size_t i = 0; i < COUNT_OF(fields); i++) {
            if(strlen(fields[i].source) == length && strncmp(line, fields[i].source, length) == 0) {
                report_append(text, "%s", fields[i].label);
                line = colon;
                break;
            }
        }
    }
    report_append(text, "%s\n", line);
}

static bool status_report_t55xx_row(ReportText* text, char* line) {
    static const char* const labels[] = {
        "Start gap", "Write gap", "Write 0", "Write 1", "Read gap", "Write 2", "Write 3"};
    char* values[COUNT_OF(labels)];
    char* end = strchr(line, '|');
    if(!end) return false;
    char* mode_end = end;
    for(size_t i = 0; i < COUNT_OF(values); i++) {
        values[i] = end + 1;
        end = strchr(values[i], '|');
        if(!end) return false;
    }
    *mode_end = '\0';
    status_report_line(text, status_report_normalize(line), false);
    for(size_t i = 0; i < COUNT_OF(values); i++) {
        *strchr(values[i], '|') = '\0';
        char* value = status_report_normalize(values[i]);
        bool row_end = i % 2 || i + 1 == COUNT_OF(values);
        report_append(text, "%s: %s%s", labels[i], value, row_end ? "\n" : "  ");
    }
    return true;
}

static void status_report_format(ReportText* text, char* raw, bool truncated) {
    bool t55xx_table = false;
    bool skip_speed = false;
    char* next = raw;
    while(next) {
        char* raw_line = next;
        next = strchr(raw_line, '\n');
        if(next) *next++ = '\0';
        bool indented = *raw_line == ' ' || *raw_line == '\t';
        char* line = status_report_normalize(raw_line);
        if(!*line) {
            t55xx_table = false;
            continue;
        }

        if(skip_speed && indented) continue;
        skip_speed = strcmp(line, "Transfer Speed") == 0;
        if(skip_speed) continue; // The app requests a zero-length speed test.

        if(t55xx_table) {
            if(line[0] == '|' || line[0] == '-' || strncmp(line, "[r] ", 4) == 0 ||
               strncmp(line, "mode |", 6) == 0) {
                continue;
            }
            if(status_report_t55xx_row(text, line)) continue;
            t55xx_table = false;
        }
        t55xx_table = strcmp(line, "LF T55XX config") == 0;
        if(line[0] == '[' && line[1] && line[2] == ']' && line[3] == ' ') line += 4;
        const char* text_line = line;
        if(t55xx_table) {
            text_line = "LF T55xx timing";
        } else if(strcmp(line, "Various") == 0) {
            text_line = "Diagnostics";
        } else if(strcmp(line, "Installed StandAlone Mode") == 0) {
            text_line = "Standalone mode";
        } else if(strcmp(line, "Flash memory dictionary loaded") == 0) {
            text_line = "Flash dictionaries";
        }
        status_report_line(text, text_line, !indented);
    }
    if(truncated || text->truncated) {
        memcpy(text->text + text->length, "\n[Report truncated]", sizeof("\n[Report truncated]"));
    }
}

static void status_report_draw(Canvas* canvas, void* context) {
    StatusReportModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, "PM5 Report");
    canvas_draw_line(canvas, 4, 14, 123, 14);

    const char* line = model->text ? model->text : model->message;
    if(!line) return;
    size_t rows = 0;
    while(*line) {
        bool heading = *line == REPORT_HEADING;
        if(heading) line++;
        const char* line_end = strchr(line, '\n');
        if(!line_end) line_end = line + strlen(line);
        canvas_set_font(canvas, heading ? FontPrimary : FontSecondary);
        while(line < line_end) {
            char row[REPORT_WIDTH + 1];
            const char* end = line;
            const char* space = NULL;
            size_t width = 0;
            while(end < line_end && (size_t)(end - line) < sizeof(row) - 1) {
                size_t glyph = canvas_glyph_width(canvas, (uint8_t)*end);
                if(end != line && width + glyph > REPORT_WIDTH) {
                    if(space && space != line) end = space;
                    break;
                }
                if(*end == ' ') space = end;
                width += glyph;
                end++;
            }
            if(rows >= model->top && rows - model->top < REPORT_ROWS) {
                size_t length = end - line;
                memcpy(row, line, length);
                row[length] = '\0';
                canvas_draw_str(canvas, 4, 24 + 9 * (rows - model->top), row);
            }
            rows++;
            line = end;
            while(line < line_end && *line == ' ') line++;
        }
        line = *line_end ? line_end + 1 : line_end;
    }
    model->rows = rows;
    if(rows > REPORT_ROWS) {
        elements_scrollbar_pos(canvas, 128, 17, 46, model->top, rows - REPORT_ROWS + 1);
    }
}

static bool status_report_input(InputEvent* event, void* context) {
    StatusReport* report = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    bool down = event->key == InputKeyDown || event->key == InputKeyRight;
    bool up = event->key == InputKeyUp || event->key == InputKeyLeft;
    if(!down && !up) return false;

    StatusReportModel* model = view_get_model(report->view);
    size_t step = event->key == InputKeyLeft || event->key == InputKeyRight ? REPORT_ROWS : 1;
    size_t max_top = model->rows > REPORT_ROWS ? model->rows - REPORT_ROWS : 0;
    size_t top = down ? MIN(model->top + step, max_top) : model->top - MIN(model->top, step);
    bool changed = top != model->top;
    model->top = top;
    view_commit_model(report->view, changed);
    return true;
}

static void status_report_show(StatusReport* report, const char* message, char* text) {
    StatusReportModel* model = view_get_model(report->view);
    free(model->text);
    model->text = text;
    model->message = message;
    model->top = 0;
    model->rows = 0;
    view_commit_model(report->view, true);
}

static int32_t status_report_worker(void* context) {
    StatusReport* report = context;
    PacketResponseNG reply;
    uint8_t wait_ms[4] = {0};
    const char* error = NULL;
    if(!proxmark5_frame_begin_operation(&report->cancel)) return 0;
    // Earlier report lines must be drained before starting capture.
    if(!proxmark5_frame_synchronize()) {
        proxmark5_frame_end_operation();
        if(!report->cancel) status_report_show(report, "No reply from PM5.", NULL);
        return 0;
    }
    char* raw = lf_capture_alloc(REPORT_CAPACITY);
    if(!raw) {
        proxmark5_frame_end_operation();
        status_report_show(report, "Not enough memory for the PM5 report.", NULL);
        return 0;
    }
    if(!proxmark5_frame_capture_start(raw, REPORT_CAPACITY)) {
        proxmark5_frame_end_operation();
        free(raw);
        status_report_show(report, "PM5 busy. Reopen to try again.", NULL);
        return 0;
    }
    clearCommandBuffer();
    if(!SendCommandNG(CMD_STATUS, wait_ms, sizeof(wait_ms))) {
        error = "Send failed. Check the PM5 connection.";
    } else if(!WaitForResponseTimeout(CMD_STATUS, &reply, 6500)) {
        error = "Report timed out. Reopen to try again.";
    } else if(reply.status != PM3_SUCCESS) {
        error = "PM5 reported a status error.";
    }

    bool truncated = false;
    proxmark5_frame_capture_end(&truncated);
    proxmark5_frame_end_operation();
    char* text = NULL;
    if(!report->cancel && !error && *raw) {
        text = lf_capture_alloc(REPORT_CAPACITY);
        if(text) {
            text[0] = '\0';
            ReportText formatted = {.text = text};
            status_report_format(&formatted, raw, truncated);
        } else {
            error = "Not enough memory for the PM5 report.";
        }
    }
    free(raw);

    StatusReportModel* model = view_get_model(report->view);
    if(report->cancel) {
        free(text);
        view_commit_model(report->view, false);
    } else {
        free(model->text);
        model->text = text;
        model->message = error ? error : "PM5 returned an empty report.";
        model->top = 0;
        model->rows = 0;
        view_commit_model(report->view, true);
    }
    return 0;
}

static void status_report_exit(void* context) {
    StatusReport* report = context;
    StatusReportModel* model = view_get_model(report->view);
    report->cancel = true;
    report->pending = false;
    free(model->text);
    model->text = NULL;
    view_commit_model(report->view, false);
}

StatusReport* status_report_create(void) {
    StatusReport* report = calloc(1, sizeof(StatusReport));
    report->view = view_alloc();
    view_allocate_model(report->view, ViewModelTypeLocking, sizeof(StatusReportModel));
    StatusReportModel* model = view_get_model(report->view);
    model->text = NULL;
    model->message = NULL;
    model->rows = 0;
    model->top = 0;
    view_commit_model(report->view, false);
    view_set_context(report->view, report);
    view_set_draw_callback(report->view, status_report_draw);
    view_set_input_callback(report->view, status_report_input);
    view_set_exit_callback(report->view, status_report_exit);
    return report;
}

void status_report_stop_worker(StatusReport* report) {
    if(!report || !report->worker) return;
    status_report_exit(report);
    furi_thread_join(report->worker);
    furi_thread_free(report->worker);
    report->worker = NULL;
}

void status_report_free(StatusReport* report) {
    status_report_stop_worker(report);
    StatusReportModel* model = view_get_model(report->view);
    free(model->text);
    view_commit_model(report->view, false);
    view_free(report->view);
    free(report);
}

View* status_report_get_view(StatusReport* report) {
    return report->view;
}

void status_report_tick(StatusReport* report) {
    if(report->worker) {
        if(furi_thread_get_state(report->worker) != FuriThreadStateStopped) return;
        furi_thread_free(report->worker);
        report->worker = NULL;
    }
    if(!report->pending) return;
    report->pending = false;
    report->cancel = false;
    report->worker = furi_thread_alloc_ex("PM5Report", 4096, status_report_worker, report);
    furi_thread_start(report->worker);
}

void status_report_start(StatusReport* report) {
    status_report_show(report, "Reading PM5 report...", NULL);
    report->pending = true;
    status_report_tick(report);
}
