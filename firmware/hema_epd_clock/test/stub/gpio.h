/*
 * stub/gpio.h - enough of the SDK's GPIO header to compile the renderer.
 *
 * epd_gfx.h pulls in epd_ssd1680.h for the panel geometry, and that in turn
 * includes the SDK's gpio.h for the pin definitions of a panel the host build
 * never talks to. Only the types are needed - nothing here is called.
 */
#ifndef _STUB_GPIO_H_
#define _STUB_GPIO_H_

#include <stdint.h>

typedef enum {
    GPIO_PORT_0, GPIO_PORT_1, GPIO_PORT_2, GPIO_PORT_3
} GPIO_PORT;

typedef enum {
    GPIO_PIN_0, GPIO_PIN_1, GPIO_PIN_2, GPIO_PIN_3, GPIO_PIN_4,
    GPIO_PIN_5, GPIO_PIN_6, GPIO_PIN_7, GPIO_PIN_8, GPIO_PIN_9
} GPIO_PIN;

#endif
