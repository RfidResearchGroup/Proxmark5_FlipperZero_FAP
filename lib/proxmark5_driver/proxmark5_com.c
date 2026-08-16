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

// OLD reply = 544B + len prefix; NG MIX with chk payload can be ~526B.
#define BUFFER_SIZE 600
#define PM3_SUCCESS 0
#define PM3_EIO     -8

typedef struct {
    // Handshake state is updated in SPI RX thread and polled by handshake caller.
    volatile bool handshake_waiting;
    volatile bool handshake_matched;
    uint8_t handshake_state;
    // Only clock SPI while a reply is expected (idle polling desyncs PM5 slave)
    volatile uint32_t rx_deadline_tick;

    // Thread for handling SPI communication
    FuriThread* thread_rx_spi;
    bool thread_running;

    // RX packet buffer stored in context (heap-backed via com_context)
    uint8_t packet[BUFFER_SIZE];
} Proxmark5ComContext;

// Global context for proxmark5 communication, it will be initialized in proxmark5_com_init
//  and deinitialized in proxmark5_com_deinit
static Proxmark5ComContext* com_context = NULL;

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

static bool proxmark5_com_handle_response_blob(uint8_t* blob, size_t blob_len) {
    if(!blob || blob_len < 12) {
        return false;
    }

    for(size_t i = 0; i + 12 <= blob_len; i++) {
        if(blob[i] != 0x50 || blob[i + 1] != 0x4d ||
           blob[i + 2] != 0x33 || blob[i + 3] != 0x62) {
            continue;
        }

        uint16_t payload_len =
            ((uint16_t)blob[i + 4] | ((uint16_t)blob[i + 5] << 8)) & 0x7FFFU;
        size_t frame_len = 12U + payload_len;
        if(i + frame_len > blob_len) {
            return false;
        }
        return proxmark5_frame_handle_packet(blob + i, frame_len);
    }

    uint16_t prefixed_len = (uint16_t)blob[0] | ((uint16_t)blob[1] << 8);
    if(prefixed_len >= 12 && (size_t)(prefixed_len + 2) <= blob_len) {
        return proxmark5_frame_handle_packet(blob + 2, prefixed_len);
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

    while(com_context->thread_running) {
        bool expect_rx = com_context->handshake_waiting ||
                         (furi_get_tick() < com_context->rx_deadline_tick);
        if(!expect_rx) {
            furi_delay_ms(20);
            continue;
        }

        // Handshake: short clock is enough for {0x04,0x00,'y','e','s',0x00}
        if(com_context->handshake_waiting) {
            uint16_t data_length = 0;
            if(!proxmark5_spi_receive_packet(
                   com_context->packet, BUFFER_SIZE, &data_length, 80)) {
                furi_delay_ms(15);
                continue;
            }
            (void)proxmark5_com_update_handshake_match(com_context->packet, data_length);
            continue;
        }

        // Command reply: one CS, one burst. Parsing a 2-byte length then
        // releasing CS on 0x0000 ate the PM5 TX FIFO (logs: incomplete 3/266).
        const size_t blob_len = BUFFER_SIZE;
        if(!proxmark5_spi_receive_data(com_context->packet, blob_len, 200)) {
            furi_delay_ms(20);
            continue;
        }

        if(proxmark5_com_update_handshake_match(com_context->packet, blob_len)) {
            com_context->rx_deadline_tick = 0;
            continue;
        }

        bool handled = proxmark5_com_handle_response_blob(com_context->packet, blob_len);
        if(handled) {
            com_context->rx_deadline_tick = 0;
        } else {
            furi_delay_ms(20);
        }
    }

    return 0;
}

// Start the proxmark5 communication thread for handling SPI reception
static void proxmark5_com_rx_spi_thread_start(void) {
    com_context->thread_rx_spi =
        furi_thread_alloc_ex("PM5_SPI_RX", 4096, proxmark5_com_spi_task, NULL);
    furi_check(com_context->thread_rx_spi != NULL);
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
    com_context->rx_deadline_tick = 0;

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
    proxmark5_cc_ctrl_deinit();
    proxmark5_uart_deinit();
    proxmark5_spi_deinit();
    // Ensure OTG is disabled to cut off power to proxmark5
    if(furi_hal_power_is_otg_enabled()) {
        furi_hal_power_disable_otg();
    }
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
bool proxmark5_com_send_spi(uint8_t* data, size_t length) {
    if(com_context == NULL || data == NULL || length == 0 || length > BUFFER_SIZE) {
        return false;
    }

    // One contiguous SPI transaction: [len_lo][len_hi][payload...][0x00]
    // Fragmented TX caused PM5 to read the length before the payload arrived → timeouts.
    size_t frame_len = 2 + length + 1;
    uint8_t* frame = malloc(frame_len);
    if(!frame) {
        return false;
    }
    frame[0] = (uint8_t)(length & 0xFF);
    frame[1] = (uint8_t)((length >> 8) & 0xFF);
    memcpy(frame + 2, data, length);
    frame[2 + length] = 0x00;

    const size_t blob_len = BUFFER_SIZE;
    uint8_t* response_blob = malloc(blob_len);
    if(!response_blob) {
        free(frame);
        return false;
    }

    // Stop fallback RX before using the SPI bus. Keep the synchronous response
    // in a private buffer so the RX thread cannot overwrite it while parsing.
    com_context->rx_deadline_tick = 0;
    bool ok = proxmark5_spi_send_then_receive(
        frame, frame_len, response_blob, blob_len, 2000);
    free(frame);
    if(!ok) {
        free(response_blob);
        FURI_LOG_W(PROXMARK5_LOG_TAG, "SPI send/recv failed (%u bytes)", (unsigned)frame_len);
        // Still listen briefly — long commands reply later via RX thread.
        com_context->rx_deadline_tick = furi_get_tick() + 2000;
        return false;
    }

    bool handled = proxmark5_com_handle_response_blob(response_blob, blob_len);
    free(response_blob);
    if(!handled) {
        // No immediate reply (normal for chkkeys_fast). RX thread continues.
        com_context->rx_deadline_tick = furi_get_tick() + 2000;
    }
    return true;
}

void proxmark5_com_expect_rx(uint32_t timeout_ms) {
    if(com_context == NULL) {
        return;
    }
    uint32_t until = furi_get_tick() + timeout_ms;
    if(until > com_context->rx_deadline_tick) {
        com_context->rx_deadline_tick = until;
    }
}
