#include <furi_hal.h>
#include <string.h>
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "proxmark5_uart.h"
#include "proxmark5_spi.h"

// PC3 is connect to the proxmark5 CC line, Set it high to ensure F0 as host
#define PROXMARK5_GPIO_CC            &gpio_ext_pc3
#define PROXMARK5_UART_HANDSHAKE_MSG "iamf0rupm5"
#define PROXMARK5_LOG_TAG            "Proxmark5_COM"

#define BUFFER_SIZE 600 // Proxmark5 data packet max size is 512bytes.
#define PM3_SUCCESS 0
#define PM3_EIO     -8

typedef struct {
    // Handshake state is updated in SPI RX thread and polled by handshake caller.
    volatile bool handshake_waiting;
    volatile bool handshake_matched;
    uint8_t handshake_state;

    // Thread for handling SPI communication
    FuriThread* thread_rx_spi;
    bool thread_running;

    // RX packet buffer stored in context (heap-backed via com_context)
    uint8_t packet[BUFFER_SIZE];
} Proxmark5ComContext;

// Global context for proxmark5 communication, it will be initialized in proxmark5_com_init
//  and deinitialized in proxmark5_com_deinit
static Proxmark5ComContext* com_context = NULL;

static uint16_t pm5_u16_le(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

// Returns true when "yes" handshake response has been fully matched.
static bool proxmark5_com_update_handshake_match(const uint8_t* data, size_t len) {
    if(!com_context->handshake_waiting) {
        return false;
    }

    for(size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if((com_context->handshake_state == 0 && c == 'y') ||
           (com_context->handshake_state == 1 && c == 'e') ||
           (com_context->handshake_state == 2 && c == 's')) {
            com_context->handshake_state++;
        } else {
            com_context->handshake_state = (c == 'y') ? 1 : 0;
        }

        if(com_context->handshake_state == 3) {
            com_context->handshake_matched = true;
            com_context->handshake_waiting = false;
            com_context->handshake_state = 0;
            FURI_LOG_I(PROXMARK5_LOG_TAG, "Received SPI handshake response: yes");
            return true;
        }
    }

    return false;
}

// proxmark5 communication control initialization, sets up the GPIO pin for CC control
static void proxmark5_cc_ctrl_init(void) {
    furi_hal_gpio_init_simple(PROXMARK5_GPIO_CC, GpioModeOutputPushPull);
    furi_hal_gpio_write(PROXMARK5_GPIO_CC, true);
}

// Deinitialize proxmark5 communication control, resets the GPIO pin to input state
static void proxmark5_cc_ctrl_deinit(void) {
    furi_hal_gpio_init_simple(PROXMARK5_GPIO_CC, GpioModeInput);
}

// Receive data from proxmark5 over SPI and cache parsed response frames.
static int32_t proxmark5_com_spi_task(void* context) {
    UNUSED(context);
    uint8_t spi_len_header[2];

    while(com_context->thread_running) {
        // Unconditional idle gap before every poll, including the success
        // path. Without this, this loop hammers the bus continuously -
        // furi_hal_spi_bus_trx() with a NULL tx buffer sends real 0xFF
        // filler bytes over MOSI on every single cycle (confirmed by
        // reading furi_hal_spi.c), so an unthrottled loop means PM5's SPI1
        // slave sees a near-constant stream of real CS pulses + 0xFF bytes
        // completely unrelated to any actual command, whether or not this
        // poll's own read this iteration succeeds or fails. That's a
        // plausible cause of the PM5-side single-byte-then-stall reception
        // failures seen when a real command is sent concurrently.
        furi_delay_ms(20);

        // Reset length header before spi receive to avoid processing stale length in case of timeout
        spi_len_header[0] = 0;
        spi_len_header[1] = 0;

        // One acquire/release for the whole frame (length header + payload),
        // NOT one per piece - see proxmark5_com_send_spi()'s comment for why:
        // the PM5's SPI1 hardware-CS slave mode starts a fresh frame on every
        // CS assertion, so releasing between the header and payload reads
        // (the old behaviour) made the PM5 see two unrelated transactions
        // instead of one logical reply.
        proxmark5_spi_acquire();

        if(!proxmark5_spi_trx_raw(spi_len_header, sizeof(spi_len_header), 100)) {
            proxmark5_spi_release();
            furi_delay_ms(100);
            continue;
        }

        uint16_t data_length = pm5_u16_le(spi_len_header);
        if(data_length == 0 || data_length > BUFFER_SIZE) {
            proxmark5_spi_release();
            if(data_length > 0) {
                FURI_LOG_W(PROXMARK5_LOG_TAG, "Invalid SPI packet length: %u", data_length);
            }
            furi_delay_ms(100);
            continue;
        }

        FURI_LOG_I(PROXMARK5_LOG_TAG, "Data length to receive: %u", data_length);

        bool got_payload = proxmark5_spi_trx_raw(com_context->packet, data_length, 2000);
        proxmark5_spi_release();

        if(!got_payload) {
            FURI_LOG_W(PROXMARK5_LOG_TAG, "SPI receive timeout, packet dropped");
            furi_delay_ms(100);
            continue;
        }

        if(proxmark5_com_update_handshake_match(com_context->packet, data_length)) {
            continue;
        }

        if(!proxmark5_frame_handle_packet(com_context->packet, data_length)) {
            FURI_LOG_W(PROXMARK5_LOG_TAG, "Invalid SPI response frame, dropped");
        }
    }

    return 0;
}

// Start the proxmark5 communication thread for handling SPI reception
static void proxmark5_com_rx_spi_thread_start(void) {
    com_context->thread_rx_spi =
        furi_thread_alloc_ex("PM5_SPI_RX", 2048, proxmark5_com_spi_task, NULL);
    furi_check(com_context->thread_rx_spi != NULL);
    // Default (Normal) priority sits above the OS's own timer thread, which
    // is deliberately kept lowest-priority - a thread that busy-polls this
    // continuously can starve it under load (see flipperdevices/
    // flipperzero-firmware#3380), which the maintainers say can deadlock
    // the whole system. This thread is background polling, not latency-
    // critical UI work, so it doesn't need Normal priority.
    furi_thread_set_priority(com_context->thread_rx_spi, FuriThreadPriorityLow);
    com_context->thread_running = true;
    furi_thread_start(com_context->thread_rx_spi);
}

// Stop the proxmark5 communication thread and free the resources
static void proxmark5_com_rx_spi_thread_stop(void) {
    if(com_context->thread_rx_spi) {
        com_context->thread_running = false;
        furi_thread_join(com_context->thread_rx_spi);
        furi_thread_free(com_context->thread_rx_spi);
        com_context->thread_rx_spi = NULL;
    }
}

/**
 * @brief Initialize proxmark5 communication interfaces (UART and SPI)
 * 
 */
void proxmark5_com_init(void) {
    if(com_context != NULL) {
        return; // Already initialized
    }

    com_context = malloc(sizeof(Proxmark5ComContext));
    // Thread for RX COM
    com_context->thread_rx_spi = NULL;
    com_context->thread_running = false;
    com_context->handshake_waiting = false;
    com_context->handshake_matched = false;
    com_context->handshake_state = 0;

    proxmark5_frame_init();

    proxmark5_cc_ctrl_init();
    proxmark5_uart_init();
    proxmark5_spi_init();
    proxmark5_com_rx_spi_thread_start();
}

/**
 * @brief Deinitialize proxmark5 communication interfaces
 * 
 */
void proxmark5_com_deinit(void) {
    proxmark5_com_rx_spi_thread_stop();
    // After the RX thread is joined, so nothing can touch the ring afterwards.
    proxmark5_frame_deinit();
    proxmark5_cc_ctrl_deinit();
    proxmark5_uart_deinit();
    proxmark5_spi_deinit();
    // Deliberately do NOT disable OTG here. Cutting 5V on every app exit
    // forces a full PM5 cold boot (reloading LF/HF key dictionaries etc,
    // tens of seconds) on every single relaunch, not just a re-handshake -
    // confirmed on hardware to take up to ~90s. Leaving OTG on keeps PM5
    // powered across relaunches so proxmark5_com_handshake()'s own
    // "already enabled" check short-circuits the wait and PM5 answers the
    // handshake almost immediately. Tradeoff: PM5 stays powered (drawing
    // Flipper battery) until OTG is turned off some other way (Flipper
    // reboot, or a future explicit "power off PM5" action) rather than
    // automatically on every app close - acceptable for a dev/debug tool,
    // matching how a wired USB client staying open doesn't power-cycle PM5
    // either.
    // Free the communication context and resources
    if(com_context) {
        free(com_context);
        com_context = NULL;
    }
}

/**
 * @brief Perform a handshake with proxmark5 to verify communication is working
 * 
 * @return true if handshake is successful, false otherwise
 */
bool proxmark5_com_handshake(void) {
    // TODO DXL: for test cc line & 5v off to make pm5 offline.
    // if(furi_hal_power_is_otg_enabled()) {
    //     furi_hal_power_disable_otg();
    // }
    // FURI_LOG_I(PROXMARK5_LOG_TAG, "Test cc line");
    // proxmark5_cc_ctrl_init();
    // furi_delay_ms(500);
    // proxmark5_cc_ctrl_deinit();
    // furi_delay_ms(500);
    // return false;

    // If OTG is not enabled, enable it to power the proxmark5(5v)
    if(!furi_hal_power_is_otg_enabled()) {
        furi_hal_power_enable_otg();
        // Wait for proxmark5 to power on and be ready
        furi_delay_ms(500);
        FURI_LOG_I(PROXMARK5_LOG_TAG, "OTG enabled to power proxmark5");
    }

    // Check IDLE or disconnected.
    if(!proxmark5_uart_is_idle()) {
        FURI_LOG_W(
            PROXMARK5_LOG_TAG,
            "UART line is busy or disconnected, please check the connection and ensure proxmark5 is powered on");
        return false;
    }

    proxmark5_frame_reset();
    com_context->handshake_waiting = true;
    com_context->handshake_matched = false;
    com_context->handshake_state = 0;

    // Send a simple start byte to wakeup/reset proxmark5
    proxmark5_uart_send_byte(0x02);
    // Send a simple handshake message over UART
    proxmark5_uart_send_string(PROXMARK5_UART_HANDSHAKE_MSG);

    // Wait for proxmark5 to process the handshake message
    furi_delay_ms(100);

    // Start time
    uint32_t start_time = furi_get_tick();
    while(furi_get_tick() - start_time < 1000) { // 1 second timeout for handshake response
        if(com_context->handshake_matched) {
            return true;
        }

        furi_delay_ms(10);
    }

    com_context->handshake_waiting = false;
    com_context->handshake_state = 0;

    return false;
}

/**
 * @brief Send a data packet over SPI to proxmark5, returns true if the packet is sent successfully
 * 
 * @param data Pointer to the data to be sent
 * @param length Length of the data to be sent
 * @return true if the packet is sent successfully
 * @return false if the packet fails to send
 */
// Sync preamble sent immediately before every frame, in the same CS-held
// transaction. PM5's software-polled SPI1 reception checks the bus only
// once per pass through its main loop, and a whole NG frame clocks by in
// well under 100 microseconds at the Flipper's fixed 2MHz SPI clock - too
// short a window for PM5 to reliably notice from an arbitrary point in its
// own loop timing. A generous run of a known sync byte first turns that
// into a multi-millisecond window: PM5 discards sync bytes until it sees
// one that doesn't match, then treats that as the real frame's first byte.
// See armsrc/pm5_cep.c's cep_spi_data_available() for the receiving half -
// SYNC_BYTE and the discard behavior must match here.
#define SYNC_BYTE 0x55
#define SYNC_LEN  500 // ~2ms at 2MHz - comfortably longer than a plausible PM5 main-loop iteration

bool proxmark5_com_send_spi(uint8_t* data, size_t length) {
    if(com_context == NULL || data == NULL || length == 0 || length > BUFFER_SIZE) {
        return false;
    }

    // Build sync preamble + length header + payload + trailer byte all in
    // ONE contiguous buffer and send it as a SINGLE furi_hal_spi_bus_tx()
    // call. Two separate tx_raw() calls back-to-back under one acquire was
    // tried first and measured (via the PM5-side debug trace) to reliably
    // truncate after only ~3 bytes of the second call - there's a real gap
    // between separate furi_hal_spi_bus_tx() calls even though CS itself
    // stays asserted throughout (acquire/release only touches CS, but
    // apparently isn't the whole story) - so, as with the original framing
    // fix, never split the transfer at all.
    uint8_t frame_buf[SYNC_LEN + 2 + BUFFER_SIZE + 1];
    memset(frame_buf, SYNC_BYTE, SYNC_LEN);
    frame_buf[SYNC_LEN + 0] = (uint8_t)(length & 0xFF);
    frame_buf[SYNC_LEN + 1] = (uint8_t)((length >> 8) & 0xFF);
    memcpy(frame_buf + SYNC_LEN + 2, data, length);
    frame_buf[SYNC_LEN + 2 + length] = 0x00; // DXL: last byte must be 0x00 to keep the proxmark5 no data rx.

    proxmark5_spi_acquire();
    bool ok = proxmark5_spi_tx_raw(frame_buf, SYNC_LEN + 2 + length + 1, 1000);
    proxmark5_spi_release();

    if(!ok) {
        FURI_LOG_W(PROXMARK5_LOG_TAG, "SPI send failed");
    }
    return ok;
}
