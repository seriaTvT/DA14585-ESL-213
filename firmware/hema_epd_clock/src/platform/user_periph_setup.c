/**
 ****************************************************************************************
 *
 * @file user_periph_setup.c
 *
 * @brief Peripherals setup and initialization.
 *
 * Copyright (C) 2015-2023 Renesas Electronics Corporation and/or its affiliates.
 * All rights reserved. Confidential Information.
 *
 * This software ("Software") is supplied by Renesas Electronics Corporation and/or its
 * affiliates ("Renesas"). Renesas grants you a personal, non-exclusive, non-transferable,
 * revocable, non-sub-licensable right and license to use the Software, solely if used in
 * or together with Renesas products. You may make copies of this Software, provided this
 * copyright notice and disclaimer ("Notice") is included in all such copies. Renesas
 * reserves the right to change or discontinue the Software at any time without notice.
 *
 * THE SOFTWARE IS PROVIDED "AS IS". RENESAS DISCLAIMS ALL WARRANTIES OF ANY KIND,
 * WHETHER EXPRESS, IMPLIED, OR STATUTORY, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. TO THE
 * MAXIMUM EXTENT PERMITTED UNDER LAW, IN NO EVENT SHALL RENESAS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE, EVEN IF RENESAS HAS BEEN ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGES. USE OF THIS SOFTWARE MAY BE SUBJECT TO TERMS AND CONDITIONS CONTAINED IN
 * AN ADDITIONAL AGREEMENT BETWEEN YOU AND RENESAS. IN CASE OF CONFLICT BETWEEN THE TERMS
 * OF THIS NOTICE AND ANY SUCH ADDITIONAL LICENSE AGREEMENT, THE TERMS OF THE AGREEMENT
 * SHALL TAKE PRECEDENCE. BY CONTINUING TO USE THIS SOFTWARE, YOU AGREE TO THE TERMS OF
 * THIS NOTICE.IF YOU DO NOT AGREE TO THESE TERMS, YOU ARE NOT PERMITTED TO USE THIS
 * SOFTWARE.
 *
 ****************************************************************************************
 */

/*
 * INCLUDE FILES
 ****************************************************************************************
 */

#include "user_periph_setup.h"
#include "datasheet.h"
#include "system_library.h"
#include "rwip_config.h"
#include "gpio.h"
#include "uart.h"
#include "syscntl.h"
#include "spi.h"
#include "epd_ssd1680.h"
#include "epd_gfx.h"

/* Draw a self-test image on the panel at boot (first-flash bring-up). Comment
 * this out once the display is proven, so normal boots skip the ~2s refresh. */
#define EPD_BOOT_TEST_PATTERN

/*
 * GLOBAL VARIABLE DEFINITIONS
 ****************************************************************************************
 */

#if DEVELOPMENT_DEBUG

void GPIO_reservations(void)
{
/*
    i.e. to reserve P0_1 as Generic Purpose I/O:
    RESERVE_GPIO(DESCRIPTIVE_NAME, GPIO_PORT_0, GPIO_PIN_1, PID_GPIO);
*/

/*
    EVERY pin later passed to GPIO_ConfigurePin() must be reserved here.
    Under DEVELOPMENT_DEBUG the SDK's pin-allocation monitor executes
    __BKPT(0) on the first unreserved pin (gpio.c, "this pin has not been
    previously reserved!"). With a debugger attached that halts the core, so
    the symptom is a firmware that appears to hang at a fixed PC inside
    GPIO_ConfigurePin - easily mistaken for a driver or hardware fault.
*/

#if defined (CFG_PRINTF_UART2)
    RESERVE_GPIO(UART2_TX, UART2_TX_PORT, UART2_TX_PIN, PID_UART2_TX);
#endif

    /* EPD SPI bus */
    RESERVE_GPIO(EPD_SCK,  SPI_CLK_PORT, SPI_CLK_PIN, PID_SPI_CLK);
    RESERVE_GPIO(EPD_MOSI, SPI_DO_PORT,  SPI_DO_PIN,  PID_SPI_DO);
    RESERVE_GPIO(EPD_MISO, SPI_DI_PORT,  SPI_DI_PIN,  PID_SPI_DI);

    /* EPD control lines - all plain GPIO (see epd_ssd1680.h) */
    RESERVE_GPIO(EPD_CS,   EPD_CS_PORT,   EPD_CS_PIN,   PID_GPIO);
    RESERVE_GPIO(EPD_DC,   EPD_DC_PORT,   EPD_DC_PIN,   PID_GPIO);
    RESERVE_GPIO(EPD_RST,  EPD_RST_PORT,  EPD_RST_PIN,  PID_GPIO);
    RESERVE_GPIO(EPD_BUSY, EPD_BUSY_PORT, EPD_BUSY_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_PWR,  EPD_PWR_PORT,  EPD_PWR_PIN,  PID_GPIO);
}

#endif

void set_pad_functions(void)
{
/*
    i.e. to set P0_1 as Generic purpose Output:
    GPIO_ConfigurePin(GPIO_PORT_0, GPIO_PIN_1, OUTPUT, PID_GPIO, false);
*/

#if defined (CFG_PRINTF_UART2)
    // Configure UART2 TX Pad (debug console over UART2, optional)
    GPIO_ConfigurePin(UART2_TX_PORT, UART2_TX_PIN, OUTPUT, PID_UART2_TX, false);
#endif

    // Configure the SPI bus pins used to talk to the EPD controller.
    // CLK = P0_0, DO/MOSI = P0_6 (both hardware SPI functions). DI/MISO is
    // unused by the write-only panel (parked on P0_2, see user_periph_setup.h).
    // CS is NOT configured here — it's driven as a plain GPIO by the EPD
    // driver (epd_gpio_init / EPD_CS in epd_ssd1680.h), on P2_1.
    GPIO_ConfigurePin(SPI_CLK_PORT, SPI_CLK_PIN, OUTPUT, PID_SPI_CLK, false);
    GPIO_ConfigurePin(SPI_DO_PORT,  SPI_DO_PIN,  OUTPUT, PID_SPI_DO,  false);
    GPIO_ConfigurePin(SPI_DI_PORT,  SPI_DI_PIN,  INPUT,  PID_SPI_DI,  false);

    // Configure the EPD's CS/DC/RST/BUSY/PWR pins (recovered from the stock
    // firmware and continuity-confirmed — see epd_ssd1680.h).
    epd_gpio_init();
}

// Configuration struct for the SPI master driving the EPD panel.
// Speed/clock-mode are conservative defaults (SSD1680 supports up to
// 20MHz SCLK, mode 0); tune once real hardware is in the loop.
static const spi_cfg_t epd_spi_cfg = {
    .spi_ms = SPI_MS_MODE_MASTER,
    .spi_cp = SPI_CP_MODE_0,
    .spi_speed = SPI_SPEED_MODE_8MHz,
    .spi_wsz = SPI_MODE_8BIT,
    .spi_cs = SPI_CS_0,
    .spi_irq = SPI_IRQ_DISABLED,
    .cs_pad = { .port = SPI_EN_PORT, .pin = SPI_EN_PIN },
};

#if defined (CFG_PRINTF_UART2)
// Configuration struct for UART2
static const uart_cfg_t uart_cfg = {
    .baud_rate = UART2_BAUDRATE,
    .data_bits = UART2_DATABITS,
    .parity = UART2_PARITY,
    .stop_bits = UART2_STOPBITS,
    .auto_flow_control = UART2_AFCE,
    .use_fifo = UART2_FIFO,
    .tx_fifo_tr_lvl = UART2_TX_FIFO_LEVEL,
    .rx_fifo_tr_lvl = UART2_RX_FIFO_LEVEL,
    .intr_priority = 2,
};
#endif

void periph_init(void)
{
#if defined (__DA14531__)
    // In Boost mode enable the DCDC converter to supply VBAT_HIGH for the used GPIOs
    syscntl_dcdc_turn_on_in_boost(SYSCNTL_DCDC_LEVEL_3V0);
#else
    // Power up peripherals' power domain
    SetBits16(PMU_CTRL_REG, PERIPH_SLEEP, 0);
    while (!(GetWord16(SYS_STAT_REG) & PER_IS_UP));
    SetBits16(CLK_16M_REG, XTAL16_BIAS_SH_ENABLE, 1);
#endif

    // ROM patch
    patch_func();

    // Initialize peripherals
#if defined (CFG_PRINTF_UART2)
    // Initialize UART2
    uart_initialize(UART2, &uart_cfg);
#endif

    // Set pad functionality
    set_pad_functions();

    // Enable the pads
    GPIO_set_pad_latch_en(true);

    // Bring up the SPI bus and run the EPD's SSD1680 init sequence.
    // NOTE: harmless to call before pairing/advertising is set up - the
    // panel just sits initialized-but-blank until the first CLEAR()/draw
    // commands arrive over the command GATT characteristic.
    spi_initialize(&epd_spi_cfg);
    epd_init(true);

#if defined (EPD_BOOT_TEST_PATTERN)
    // First-flash bring-up: draw a self-test image so you get something
    // visible on the panel immediately, without needing BLE working yet.
    // Remove the EPD_BOOT_TEST_PATTERN define (below) once the panel is
    // proven, so boot doesn't spend ~2s doing a full refresh every time.
    epd_gfx_test_pattern();
    epd_display(epd_framebuffer);
#endif
}
