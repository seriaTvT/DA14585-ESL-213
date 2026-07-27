/**
 ****************************************************************************************
 *
 * @file user_periph_setup.h
 *
 * @brief Peripherals setup header file.
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

#ifndef _USER_PERIPH_SETUP_H_
#define _USER_PERIPH_SETUP_H_

/*
 * INCLUDE FILES
 ****************************************************************************************
 */

#include "arch.h"
#include "gpio.h"
#include "uart.h"

/* Pulled in for EPD_BOARD_VARIANT_A / EPD_BITBANG: which pads the hardware SPI
 * block gets depends on the board variant, because on variant B the panel and
 * the boot flash share it and on variant A they do not. This has to come before
 * the SPI section below, so it is an include rather than a comment telling the
 * reader to define the variant first. */
#include "epd_ssd1680.h"


/*
 * DEFINES
 ****************************************************************************************
 */


/****************************************************************************************/
/* UART2 configuration to use with arch_console print messages                          */
/****************************************************************************************/
// Define UART2 Tx Pad
#if defined (__DA14531__)
    #define UART2_TX_PORT           GPIO_PORT_0
    #define UART2_TX_PIN            GPIO_PIN_6
#else
    #define UART2_TX_PORT           GPIO_PORT_0
    #define UART2_TX_PIN            GPIO_PIN_4
#endif

// Define UART2 Settings
#define UART2_BAUDRATE              UART_BAUDRATE_115200
#define UART2_DATABITS              UART_DATABITS_8
#define UART2_PARITY                UART_PARITY_NONE
#define UART2_STOPBITS              UART_STOPBITS_1
#define UART2_AFCE                  UART_AFCE_DIS
#define UART2_FIFO                  UART_FIFO_EN
#define UART2_TX_FIFO_LEVEL         UART_TX_FIFO_LEVEL_0
#define UART2_RX_FIFO_LEVEL         UART_RX_FIFO_LEVEL_0

/****************************************************************************************/
/* SPI configuration                                                                    */
/****************************************************************************************/
// Define SPI Pads
#if defined (__DA14531__)
    #define SPI_EN_PORT             GPIO_PORT_0
    #define SPI_EN_PIN              GPIO_PIN_1

    #define SPI_CLK_PORT            GPIO_PORT_0
    #define SPI_CLK_PIN             GPIO_PIN_4

    #define SPI_DO_PORT             GPIO_PORT_0
    #define SPI_DO_PIN              GPIO_PIN_0

    #define SPI_DI_PORT             GPIO_PORT_0
    #define SPI_DI_PIN              GPIO_PIN_3

#elif !defined (__DA14586__) && defined (EPD_BOARD_VARIANT_A)
    /* Variant A: the hardware SPI block serves the BOOT FLASH ONLY. The panel
     * is bit-banged on its own pins (P0_1/P2_0, see epd_ssd1680.h), so unlike
     * variant B nothing here is shared with it.
     *   CS = P0_3   CLK = P0_0   MOSI/DO = P0_6   MISO/DI = P0_5
     * DI can simply be the flash's own P0_5 here: variant B had to park it on
     * P0_2 only because P0_5 doubled as that board's D/C line. That whole
     * detach-and-restore dance in epd_store.c therefore becomes a no-op on this
     * board - it reconfigures P0_5 to the function it already has. */
    #define SPI_EN_PORT             GPIO_PORT_0
    #define SPI_EN_PIN              GPIO_PIN_3

    #define SPI_CLK_PORT            GPIO_PORT_0
    #define SPI_CLK_PIN             GPIO_PIN_0

    #define SPI_DO_PORT             GPIO_PORT_0
    #define SPI_DO_PIN              GPIO_PIN_6

    #define SPI_DI_PORT             GPIO_PORT_0
    #define SPI_DI_PIN              GPIO_PIN_5

#elif !defined (__DA14586__)
    /* EPD SPI pins recovered from the community firmware (variant B) — see
     * epd_ssd1680.h and PROTOCOL_NOTES.md §13.
     *   CS  = P2_1 (QFN pin 8)   CLK = P0_0 (pin 1)   MOSI/DO = P0_6 (pin 9)
     * The SDK's spi_cs_low()/spi_cs_high() drive SPI_EN, so setting SPI_EN to
     * P2_1 makes the driver toggle the correct chip-select line. */
    #define SPI_EN_PORT             GPIO_PORT_2
    #define SPI_EN_PIN              GPIO_PIN_1

    #define SPI_CLK_PORT            GPIO_PORT_0
    #define SPI_CLK_PIN             GPIO_PIN_0

    #define SPI_DO_PORT             GPIO_PORT_0
    #define SPI_DO_PIN              GPIO_PIN_6

    /* MISO is unused — the EPD is write-only. NOTE: the DA14585 boot ROM's
     * flash-MISO default is P0_5, which this board reuses as the EPD D/C
     * line (the stock firmware time-shares P0_5 between the boot flash and
     * the panel, since the flash and panel have separate chip-selects). So
     * SPI_DI must NOT be left on P0_5 here or it will fight EPD_DC. Parked on
     * P0_2 as a harmless don't-care; confirm P0_2 is unconnected on your
     * board, or drop the DI pad from spi_initialize() entirely. */
    #define SPI_DI_PORT             GPIO_PORT_0
    #define SPI_DI_PIN              GPIO_PIN_2
#endif

/***************************************************************************************/
/* Production debug output configuration                                               */
/***************************************************************************************/
#if PRODUCTION_DEBUG_OUTPUT
#if defined (__DA14531__)
    #define PRODUCTION_DEBUG_PORT   GPIO_PORT_0
    #define PRODUCTION_DEBUG_PIN    GPIO_PIN_11
#else
    #define PRODUCTION_DEBUG_PORT   GPIO_PORT_2
    #define PRODUCTION_DEBUG_PIN    GPIO_PIN_5
#endif
#endif

/*
 * FUNCTION DECLARATIONS
 ****************************************************************************************
 */

#if DEVELOPMENT_DEBUG
/**
 ****************************************************************************************
 * @brief   Reserves application's specific GPIOs
 * @details Used only in Development mode (#if DEVELOPMENT_DEBUG)
 *          i.e. to reserve P0_1 as Generic Purpose I/O:
 *          RESERVE_GPIO(DESCRIPTIVE_NAME, GPIO_PORT_0, GPIO_PIN_1, PID_GPIO);
 ****************************************************************************************
 */
void GPIO_reservations(void);
#endif

/**
 ****************************************************************************************
 * @brief   Sets the functionality of application pads
 * @details i.e. to set P0_1 as Generic purpose Output:
 *          GPIO_ConfigurePin(GPIO_PORT_0, GPIO_PIN_1, OUTPUT, PID_GPIO, false);
 ****************************************************************************************
 */
void set_pad_functions(void);

/**
 ****************************************************************************************
 * @brief   Initializes application's peripherals and pins
 ****************************************************************************************
 */
void periph_init(void);


#endif // _USER_PERIPH_SETUP_H_
