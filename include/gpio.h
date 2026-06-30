/* include/gpio.h — BCM2711 GPIO driver interface (Issue #4) */

#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>

/* GPIO alternate-function codes (BCM2711 GPFSEL encoding, 3 bits per pin). */
typedef enum {
    GPIO_FUNC_INPUT  = 0,
    GPIO_FUNC_OUTPUT = 1,
    GPIO_FUNC_ALT5   = 2,
    GPIO_FUNC_ALT4   = 3,
    GPIO_FUNC_ALT0   = 4,
    GPIO_FUNC_ALT1   = 5,
    GPIO_FUNC_ALT2   = 6,
    GPIO_FUNC_ALT3   = 7,
} gpio_func_t;

/* Initialise the GPIO subsystem (clears all pull-up/pull-down resistors). */
void gpio_init(void);

/* Configure the alternate function for a single GPIO pin. */
void gpio_set_function(uint32_t pin, gpio_func_t func);

/* Drive a GPIO pin high. */
void gpio_set(uint32_t pin);

/* Drive a GPIO pin low. */
void gpio_clear(uint32_t pin);

/* Read the level of a GPIO pin (1 = high, 0 = low).  Used for the CV/Gate
 * input's gate pin (issue #32). */
int gpio_get(uint32_t pin);

#endif /* GPIO_H */
