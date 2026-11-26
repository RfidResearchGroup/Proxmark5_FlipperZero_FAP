#ifndef WAVEFORM_PAGE_H
#define WAVEFORM_PAGE_H

#include <gui/view.h>

typedef struct WaveformPage WaveformPage;

typedef enum {
    WaveformLf,
} WaveformBand;

WaveformPage* waveform_page_create(void);
uint32_t waveform_page_previous(void* context);
void waveform_page_reset(WaveformPage* page, WaveformBand band, uint32_t previous_view);
bool waveform_page_browse(WaveformPage* page, WaveformBand band, uint32_t previous_view);
View* waveform_page_get_view(WaveformPage* page);
void waveform_page_wait(WaveformPage* page);
void waveform_page_free(WaveformPage* page);

#endif
