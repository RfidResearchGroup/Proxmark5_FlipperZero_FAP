#include <furi_hal.h>
#include "proxmark5_com.h"
#include "proxmark5_uart.h"
#include "proxmark5_spi.h"

// PC3 is connect to the proxmark5 CC line, Set it high to ensure F0 as host
#define PROXMARK5_GPIO_CC            &gpio_ext_pc3
#define PROXMARK5_UART_HANDSHAKE_MSG "iamf0rupm5"
#define PROXMARK5_LOG_TAG            "Proxmark5_COM"

#define BUFFER_SIZE   600 // Proxmark5 data packet max size is 512bytes.
#define TRIGGER_LEVEL 1 // Unblock the receiver when 1+ bytes are available

typedef struct {
    // Buffers for SPI communication
    FuriStreamBuffer* spi_rx_buffer;

    // Thread for handling SPI communication
    FuriThread* thread_rx_spi;
    bool thread_running;
} Proxmark5ComContext;

// Global context for proxmark5 communication, it will be initialized in proxmark5_com_init
//  and deinitialized in proxmark5_com_deinit
static Proxmark5ComContext* com_context = NULL;

// proxmark5 communication control initialization, sets up the GPIO pin for CC control
static void proxmark5_cc_ctrl_init(void) {
    furi_hal_gpio_init_simple(PROXMARK5_GPIO_CC, GpioModeOutputPushPull);
    furi_hal_gpio_write(PROXMARK5_GPIO_CC, true);
}

// Deinitialize proxmark5 communication control, resets the GPIO pin to input state
static void proxmark5_cc_ctrl_deinit(void) {
    furi_hal_gpio_init_simple(PROXMARK5_GPIO_CC, GpioModeInput);
}

// // Receive data from proxmark5 over SPI and store it in the spi_rx_buffer, until the thread is stopped
static int32_t proxmark5_com_spi_task(void* context) {
    UNUSED(context);
    uint16_t data_length = 0;
    while(com_context->thread_running) {
        uint8_t buffer[2];
        // Try to receive data with a timeout, if data is received, store it in the spi_rx_buffer
        if(proxmark5_spi_receive_data(buffer, data_length != 0 ? 1 : 2, 100)) { // 100 ms timeout
            if(data_length == 0) {
                // little endian format for data length, first receive the low byte then the high byte
                data_length = buffer[0] | (buffer[1] << 8);
                // Check if the data length is valid, if not, discard it and wait for the next one
                if(data_length == 0 || data_length > BUFFER_SIZE) {
                    data_length = 0;
                    furi_delay_ms(100); // Avoid busy loop when receiving invalid data
                    continue;
                }
            } else {
                // Store the received data in the spi_rx_buffer
                if(furi_stream_buffer_send(com_context->spi_rx_buffer, buffer, 1, 1000) != 1) {
                    FURI_LOG_W(PROXMARK5_LOG_TAG, "SPI RX buffer overflow, data lost");
                }
                data_length--;
            }
        } else {
            furi_delay_ms(100);
        }
    }

    return 0;
}

// Start the proxmark5 communication thread for handling SPI reception
static void proxmark5_com_rx_spi_thread_start(void) {
    com_context->thread_rx_spi =
        furi_thread_alloc_ex("PM5_SPI_RX", 512, proxmark5_com_spi_task, NULL);
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
    com_context->spi_rx_buffer = furi_stream_buffer_alloc(
        BUFFER_SIZE, TRIGGER_LEVEL); // Allocate a buffer for SPI reception
    furi_check(com_context->spi_rx_buffer != NULL);
    // Thread for RX COM
    com_context->thread_rx_spi = NULL;
    com_context->thread_running = false;

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
        if(com_context->spi_rx_buffer) {
            furi_stream_buffer_free(com_context->spi_rx_buffer);
            com_context->spi_rx_buffer = NULL;
        }
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

    // Clear the SPI RX buffer before starting the handshake
    furi_stream_buffer_reset(com_context->spi_rx_buffer);

    // Send a simple start byte to wakeup/reset proxmark5
    proxmark5_uart_send_byte(0x02);
    // Send a simple handshake message over UART
    proxmark5_uart_send_string(PROXMARK5_UART_HANDSHAKE_MSG);

    // Wait for proxmark5 to process the handshake message
    furi_delay_ms(100);

    // Rx response by SPI, expecting "yes" from proxmark5
    uint8_t buffer[4] = {0}; // 3 bytes + null terminator

    // Start time
    uint32_t start_time = furi_get_tick();
    while(furi_get_tick() - start_time < 1000) { // 1 second timeout for handshake response
        // Wait until the expected number of bytes are available in the SPI RX buffer
        if(furi_stream_buffer_bytes_available(com_context->spi_rx_buffer) != sizeof(buffer)) {
            continue;
        }
        size_t rx_len =
            furi_stream_buffer_receive(com_context->spi_rx_buffer, buffer, sizeof(buffer), 100);
        if(rx_len == sizeof(buffer)) {
            FURI_LOG_I(PROXMARK5_LOG_TAG, "Received SPI response: %s", buffer);
            if(strcmp((char*)buffer, "yes") == 0) {
                return true;
            } else {
                return false;
            }
        }
    }

    return false;
}
