#ifndef SIMPLE_READERS_H
#define SIMPLE_READERS_H

#include <stddef.h>
#include <stdint.h>
#include <furi.h>

#define SIMPLE_READER_MAX_LINES 3U
#define SIMPLE_READER_LINE_SIZE 32U
#define EM4100_ID_BYTES 5U


typedef struct {
    char lines[SIMPLE_READER_MAX_LINES][SIMPLE_READER_LINE_SIZE];
    size_t line_count;
    uint8_t* data;
} SimpleReaderResult;

typedef struct {
    const char* title;
    const char* prompt;
    const char* no_data;
    const char* no_data_hint;
    size_t data_size;
    int (*read)(SimpleReaderResult* result);
    void (*save)(const uint8_t* data);
    void (*format)(SimpleReaderResult* result);
    void (*format_details)(const uint8_t* data, FuriString* text);
} SimpleReader;

extern const SimpleReader simple_reader_ntag213;
extern const SimpleReader* const simple_readers[];
extern const size_t simple_reader_count;

#endif
