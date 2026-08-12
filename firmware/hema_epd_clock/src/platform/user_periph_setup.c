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
#include "epd_board.h"

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

    /* Hardware SPI pads. On variant B these are shared with the panel; on
     * variant A they belong to the boot flash alone (see user_periph_setup.h). */
    RESERVE_GPIO(SPI_CLK,  SPI_CLK_PORT, SPI_CLK_PIN, PID_SPI_CLK);
    RESERVE_GPIO(SPI_MOSI, SPI_DO_PORT,  SPI_DO_PIN,  PID_SPI_DO);
    RESERVE_GPIO(SPI_MISO, SPI_DI_PORT,  SPI_DI_PIN,  PID_SPI_DI);

#if EPD_BITBANG && !EPD_PANEL_PADS_SHARED
    /* The panel's clock and data, as plain GPIOs, where they are pads of their
     * own. Both variants bit-bang, but variant B does it on the three pads
     * already reserved above, and reserving a pin twice is not harmless here:
     * RESERVE_GPIO sets the slot to -1 on the second call and then shifts that
     * into the mask, which turns GPIO_status into a smear of set bits and
     * disarms the monitor for pins that really were never reserved. GPIO_init()
     * catches it with its own __BKPT(0) ("this pin has been previously
     * reserved!"), which presents as a hang at boot - the same symptom the note
     * above warns about, from the opposite cause. */
    RESERVE_GPIO(EPD_SCK, EPD_SCK_PORT, EPD_SCK_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_SDA, EPD_SDA_PORT, EPD_SDA_PIN, PID_GPIO);
#endif

    /* Variant A's spare enable line. Named only on that board - see
     * epd_gpio_init(). */
#ifdef EPD_AUX_PORT
    RESERVE_GPIO(EPD_AUX, EPD_AUX_PORT, EPD_AUX_PIN, PID_GPIO);
#endif

    /* EPD control lines - all plain GPIO (see epd_ssd1680.h) */
    RESERVE_GPIO(EPD_CS,   EPD_CS_PORT,   EPD_CS_PIN,   PID_GPIO);
    RESERVE_GPIO(EPD_DC,   EPD_DC_PORT,   EPD_DC_PIN,   PID_GPIO);
    RESERVE_GPIO(EPD_RST,  EPD_RST_PORT,  EPD_RST_PIN,  PID_GPIO);
    RESERVE_GPIO(EPD_BUSY, EPD_BUSY_PORT, EPD_BUSY_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_PWR,  EPD_PWR_PORT,  EPD_PWR_PIN,  PID_GPIO);

    /* Boot-flash chip select, driven by epd_store.c when it borrows the bus to
     * persist the template. Every pin passed to GPIO_ConfigurePin() has to be
     * reserved or the SDK's allocation monitor hits __BKPT(0), which looks
     * exactly like a driver hang. */
    RESERVE_GPIO(FLASH_CS, GPIO_PORT_0, GPIO_PIN_3, PID_SPI_EN);
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

    // Configure the hardware SPI bus pins. On variant B these carry the panel
    // as well as the boot flash; on variant A they are the flash's alone and
    // the panel is bit-banged elsewhere (see user_periph_setup.h).
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

    /* Ask the board what it is, before anything drives a panel pin. Reads the
     * record at flash 0x039000 and only records what it found; see
     * epd/epd_board.h. Must come before epd_spi_claim(), because it takes the
     * flash bus - and hands it back via epd_spi_claim() itself. */
    epd_board_check();

    // Bring up the SPI bus and run the EPD's SSD1680 init sequence.
    // NOTE: harmless to call before pairing/advertising is set up - the
    // panel just sits initialized-but-blank until the first CLEAR()/draw
    // commands arrive over the command GATT characteristic.
    epd_spi_claim();

    /* Refusing to drive a panel the board says we are not built for.
     *
     * OFF BY DEFAULT, and the reason is worth stating rather than assuming.
     * The verdict rests on one inference from three tags - that a written pin
     * map means variant A, and an erased record means variant B. That
     * inference has a known way to be wrong: tools/mksuota.py --no-fallback
     * synthesises an image with 0x039000 erased, so OUR OWN tooling can turn a
     * variant-A board into one that claims to be variant B. Enforcing on that
     * would leave a working tag blank, which is the failure this whole check
     * exists to prevent.
     *
     * What it protects when enabled is real but one-directional. A variant-A
     * build drives P2_0 as SDA; on a variant-B board that pin is BUSY, a panel
     * output, so the two fight. The other way round is harmless - our
     * variant-B map never touches P1_1, which is variant A's only panel
     * output. So the case worth refusing is a variant-A build on a board that
     * says variant B, and that is exactly what a mismatch means here. */
#if defined(EPD_BOARD_CHECK) && (EPD_BOARD_CHECK)
    if (epd_board_verdict() == EPD_BOARD_MISMATCH) {
        return;         /* keeps time and stays reachable over BLE */
    }
#endif

    /* Ask the panel whether it is there before driving it. The answer is only
     * recorded, never acted on - see epd_panel_present(). A tag whose flex has
     * come loose should still keep time and stay reachable over BLE. */
#if EPD_BITBANG && EPD_PANEL_PROBE
    (void)epd_panel_present();
#endif

    epd_init(true);
}
