// HAL subset required by the official CT117E LCD BSP, implemented directly
// on the STM32G431 registers (no HAL library, no host-side shortcuts).
#include "main.h"

volatile uint32_t g_uwTick = 0;

void HAL_GPIO_Init(GPIO_TypeDef* port, GPIO_InitTypeDef* init) {
    for (int pin = 0; pin < 16; pin++) {
        const uint32_t bit = (1u << pin);
        if ((init->Pin & bit) == 0) continue;

        const uint32_t shift = (uint32_t)pin * 2u;

        // MODER: input = 00, push-pull output = 01 (RM0440 8.4.1)
        uint32_t mode = (init->Mode == GPIO_MODE_INPUT) ? 0u : 1u;
        port->MODER = (port->MODER & ~(3u << shift)) | (mode << shift);

        // OTYPER: push-pull = 0
        port->OTYPER &= ~bit;

        // OSPEEDR
        port->OSPEEDR = (port->OSPEEDR & ~(3u << shift)) |
                        ((init->Speed & 3u) << shift);

        // PUPDR
        port->PUPDR = (port->PUPDR & ~(3u << shift)) |
                      ((init->Pull & 3u) << shift);
    }
}

// Millisecond delay driven by the 1 kHz SysTick (same contract as HAL_Delay:
// the firmware really waits on the time base, no host shortcut).
void HAL_Delay(uint32_t ms) {
    const uint32_t start = g_uwTick;
    while ((g_uwTick - start) < ms) {
    }
}