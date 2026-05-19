#include <furi_hal.h>
#include "proxmark5_com.h"
#include "proxmark5_uart.h"
#include "proxmark5_spi.h"

// PC3 is connect to the proxmark5 CC line, Set it high to ensure F0 as host
#define PROXMARK5_GPIO_CC            &gpio_ext_pc3
#define PROXMARK5_UART_HANDSHAKE_MSG "iamf0rupm5"
#define PROXMARK5_LOG_TAG            "Proxmark5_COM"

// proxmark5 communication control initialization, sets up the GPIO pin for CC control
static void proxmark5_cc_ctrl_init(void) {
    furi_hal_gpio_init_simple(PROXMARK5_GPIO_CC, GpioModeOutputPushPull);
    furi_hal_gpio_write(PROXMARK5_GPIO_CC, true);
}

// Deinitialize proxmark5 communication control, resets the GPIO pin to input state
static void proxmark5_cc_ctrl_deinit(void) {
    furi_hal_gpio_init_simple(PROXMARK5_GPIO_CC, GpioModeInput);
}

/**
 * @brief Initialize proxmark5 communication interfaces (UART and SPI)
 * 
 */
void proxmark5_com_init(void) {
    proxmark5_cc_ctrl_init();
    proxmark5_uart_init();
    proxmark5_spi_init();
}

/**
 * @brief Deinitialize proxmark5 communication interfaces
 * 
 */
void proxmark5_com_deinit(void) {
    proxmark5_cc_ctrl_deinit();
    proxmark5_uart_deinit();
    proxmark5_spi_deinit();
    // Ensure OTG is disabled to cut off power to proxmark5
    if(furi_hal_power_is_otg_enabled()) {
        furi_hal_power_disable_otg();
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

    // Send a simple start byte to wakeup/reset proxmark5
    proxmark5_uart_send_byte(0x02);
    // Send a simple handshake message over UART
    proxmark5_uart_send_string(PROXMARK5_UART_HANDSHAKE_MSG);

    // Wait for proxmark5 to process the handshake message
    furi_delay_ms(100);

    // Rx response by SPI, expecting "yes" from proxmark5
    uint8_t buffer[4] = {0}; // 3 bytes + null terminator
    // Wait for response with a timeout, if received "yes" then handshake is successful
    if(proxmark5_spi_receive_data(buffer, sizeof(buffer) - 1, 1000)) { // 1 second timeout
        FURI_LOG_I(PROXMARK5_LOG_TAG, "Received SPI response: %s", buffer);
        if(strcmp((char*)buffer, "yes") == 0) {
            return true;
        }
    }
    return false;
}
