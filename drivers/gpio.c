/* drivers/gpio.c — BCM2711 GPIO driver (Issue #4)
 *
 * Target: Raspberry Pi CM4 / Pi 4 (BCM2711, Cortex-A72).
 *
 * The CM4 on-board LED is on GPIO 42 (active-high).
 *
 * BCM2711 peripheral base: 0xFE000000.
 * GPIO block starts at offset 0x200000 → base address 0xFE200000.
 *
 * BCM2711 uses GPIO_PUP_PDN_CNTRL_REGn (not the older GPPUD/GPPUDCLK
 * scheme used on BCM2835/BCM2837) for pull-up/pull-down configuration.
 */

#include "gpio.h"
#include <stdint.h>

#define GPIO_BASE   0xFE200000UL

/* GPFSEL0-5: function-select, 10 pins per register, 3 bits per pin. */
#define GPFSEL(n)   (*(volatile uint32_t *)(GPIO_BASE + (n) * 4u))

/* Output-set registers: GPSET0 for pins 0-31, GPSET1 for pins 32-53. */
#define GPSET0      (*(volatile uint32_t *)(GPIO_BASE + 0x1Cu))
#define GPSET1      (*(volatile uint32_t *)(GPIO_BASE + 0x20u))

/* Output-clear registers: GPCLR0 for pins 0-31, GPCLR1 for pins 32-53. */
#define GPCLR0      (*(volatile uint32_t *)(GPIO_BASE + 0x28u))
#define GPCLR1      (*(volatile uint32_t *)(GPIO_BASE + 0x2Cu))

/* BCM2711 pull-up/pull-down: 2 bits per pin, 16 pins per 32-bit register.
 * Registers at offsets 0xE4 (pins 0-15), 0xE8 (16-31), 0xEC (32-47),
 * 0xF0 (48-53).  Values: 0=no pull, 1=pull-up, 2=pull-down. */
#define GPIO_PUP_PDN_REG(n) (*(volatile uint32_t *)(GPIO_BASE + 0xE4u + (n) * 4u))

void gpio_set_function(uint32_t pin, gpio_func_t func)
{
    uint32_t reg   = pin / 10u;
    uint32_t shift = (pin % 10u) * 3u;
    uint32_t val   = GPFSEL(reg);
    val &= ~(0x7u << shift);
    val |=  ((uint32_t)func << shift);
    GPFSEL(reg) = val;
}

void gpio_set(uint32_t pin)
{
    if (pin < 32u)
        GPSET0 = 1u << pin;
    else
        GPSET1 = 1u << (pin - 32u);
}

void gpio_clear(uint32_t pin)
{
    if (pin < 32u)
        GPCLR0 = 1u << pin;
    else
        GPCLR1 = 1u << (pin - 32u);
}

/* Pin-level registers: GPLEV0 for pins 0-31, GPLEV1 for pins 32-53. */
#define GPLEV0      (*(volatile uint32_t *)(GPIO_BASE + 0x34u))
#define GPLEV1      (*(volatile uint32_t *)(GPIO_BASE + 0x38u))

int gpio_get(uint32_t pin)
{
    if (pin < 32u)
        return (int)((GPLEV0 >> pin) & 1u);
    return (int)((GPLEV1 >> (pin - 32u)) & 1u);
}

void gpio_init(void)
{
    /* Clear all pull-up/pull-down resistors on BCM2711 (54 GPIO pins
     * span four 32-bit GPIO_PUP_PDN_CNTRL_REG registers). */
    for (int i = 0; i < 4; i++)
        GPIO_PUP_PDN_REG(i) = 0u;
}
