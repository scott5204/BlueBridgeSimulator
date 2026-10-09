/*
  Minimal HAL shim for the CT117E-M4 LCD test firmware.

  The official CT117E LCD BSP (lcd.c / lcd.h / fonts.h, delivered in
  蓝桥杯LCD驱动/) is compiled VERBATIM -- it keeps using the real
  register-level GPIO sequences (BRR/BSRR/ODR/IDR) that the simulator has to
  see. Only the handful of HAL symbols the BSP references are provided here,
  implemented directly on the STM32G431 registers.

  Register layout follows the ST CMSIS stm32g431xx.h (GPIO BRR exists on
  STM32G4 at offset 0x28, so the BSP's `GPIOB->BRR |= ...` works unchanged).
*/
#ifndef __MAIN_H
#define __MAIN_H

#include <stdint.h>

#define __IO volatile

/* ---- GPIO ---- */
typedef struct {
    __IO uint32_t MODER;
    __IO uint32_t OTYPER;
    __IO uint32_t OSPEEDR;
    __IO uint32_t PUPDR;
    __IO uint32_t IDR;
    __IO uint32_t ODR;
    __IO uint32_t BSRR;
    __IO uint32_t LCKR;
    __IO uint32_t AFR[2];
    __IO uint32_t BRR;
} GPIO_TypeDef;

#define GPIOA ((GPIO_TypeDef*)0x48000000u)
#define GPIOB ((GPIO_TypeDef*)0x48000400u)
#define GPIOC ((GPIO_TypeDef*)0x48000800u)
#define GPIOD ((GPIO_TypeDef*)0x48000C00u)
#define GPIOE ((GPIO_TypeDef*)0x48001000u)

typedef struct {
    uint32_t Pin;
    uint32_t Mode;
    uint32_t Speed;
    uint32_t Pull;
} GPIO_InitTypeDef;

#define GPIO_MODE_INPUT 0x00000000u
#define GPIO_MODE_OUTPUT_PP 0x00000001u
#define GPIO_SPEED_FREQ_LOW 0x00000000u
#define GPIO_SPEED_FREQ_MEDIUM 0x00000001u
#define GPIO_SPEED_FREQ_HIGH 0x00000002u
#define GPIO_SPEED_FREQ_VERY_HIGH 0x00000003u
#define GPIO_NOPULL 0x00000000u
#define GPIO_PULLUP 0x00000001u
#define GPIO_PULLDOWN 0x00000002u

#define GPIO_PIN_0 ((uint32_t)0x0001u)
#define GPIO_PIN_1 ((uint32_t)0x0002u)
#define GPIO_PIN_2 ((uint32_t)0x0004u)
#define GPIO_PIN_3 ((uint32_t)0x0008u)
#define GPIO_PIN_4 ((uint32_t)0x0010u)
#define GPIO_PIN_5 ((uint32_t)0x0020u)
#define GPIO_PIN_6 ((uint32_t)0x0040u)
#define GPIO_PIN_7 ((uint32_t)0x0080u)
#define GPIO_PIN_8 ((uint32_t)0x0100u)
#define GPIO_PIN_9 ((uint32_t)0x0200u)
#define GPIO_PIN_10 ((uint32_t)0x0400u)
#define GPIO_PIN_11 ((uint32_t)0x0800u)
#define GPIO_PIN_12 ((uint32_t)0x1000u)
#define GPIO_PIN_13 ((uint32_t)0x2000u)
#define GPIO_PIN_14 ((uint32_t)0x4000u)
#define GPIO_PIN_15 ((uint32_t)0x8000u)
#define GPIO_PIN_All ((uint32_t)0xFFFFu)

/* ---- RCC (AHB2 peripheral clock enable, RM0440 0x4C) ---- */
#define RCC_AHB2ENR (*(volatile uint32_t*)0x4002104Cu)
#define __HAL_RCC_GPIOA_CLK_ENABLE() (RCC_AHB2ENR |= (1u << 0))
#define __HAL_RCC_GPIOB_CLK_ENABLE() (RCC_AHB2ENR |= (1u << 1))
#define __HAL_RCC_GPIOC_CLK_ENABLE() (RCC_AHB2ENR |= (1u << 2))

static inline void __nop(void) { __asm volatile("nop"); }

/* ---- HAL subset used by the LCD BSP (see hal_shim.c) ---- */
void HAL_GPIO_Init(GPIO_TypeDef* port, GPIO_InitTypeDef* init);
void HAL_Delay(uint32_t ms);
extern volatile uint32_t g_uwTick;  /* 1 kHz SysTick counter (ms) */

#endif /* __MAIN_H */