/*
  Minimal register shim for the USART1 test firmware (bare metal, direct MMIO).

  Layout follows the ST CMSIS stm32g431xx.h (USART_TypeDef offsets, RCC
  register offsets, NVIC/SCB/SysTick). Nothing here uses a HAL: the firmware
  programs the same registers a CubeMX/LL project would.
*/
#ifndef __MAIN_H
#define __MAIN_H

#include <stdint.h>

#define __IO volatile

/* ---- GPIO (AHB2) ---- */
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

/* ---- RCC (AHB1) ---- */
#define RCC_BASE 0x40021000u
#define RCC_CR (*(volatile uint32_t*)(RCC_BASE + 0x00u))
#define RCC_CFGR (*(volatile uint32_t*)(RCC_BASE + 0x08u))
#define RCC_PLLCFGR (*(volatile uint32_t*)(RCC_BASE + 0x0Cu))
#define RCC_AHB2ENR (*(volatile uint32_t*)(RCC_BASE + 0x4Cu))
#define RCC_APB2ENR (*(volatile uint32_t*)(RCC_BASE + 0x60u))
#define RCC_APB2ENR_USART1EN (1u << 14)

#define FLASH_ACR (*(volatile uint32_t*)0x40022000u)

/* ---- USART (APB2). USART1 base / IRQn from CMSIS: 0x40013800, 37. ---- */
typedef struct {
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t CR3;
    __IO uint32_t BRR;
    __IO uint32_t GTPR;
    __IO uint32_t RTOR;
    __IO uint32_t RQR;
    __IO uint32_t ISR;
    __IO uint32_t ICR;
    __IO uint32_t RDR;
    __IO uint32_t TDR;
    __IO uint32_t PRESC;
} USART_TypeDef;

#define USART1 ((USART_TypeDef*)0x40013800u)

/* CR1 */
#define USART_CR1_UE (1u << 0)
#define USART_CR1_RE (1u << 2)
#define USART_CR1_TE (1u << 3)
#define USART_CR1_RXNEIE (1u << 5)
#define USART_CR1_TCIE (1u << 6)
#define USART_CR1_TXEIE (1u << 7)
/* ISR */
#define USART_ISR_PE (1u << 0)
#define USART_ISR_FE (1u << 1)
#define USART_ISR_NE (1u << 2)
#define USART_ISR_ORE (1u << 3)
#define USART_ISR_IDLE (1u << 4)
#define USART_ISR_RXNE (1u << 5)
#define USART_ISR_TC (1u << 6)
#define USART_ISR_TXE (1u << 7)
/* ICR (write 1 to clear) */
#define USART_ICR_ORECF (1u << 3)
#define USART_ICR_TCCF (1u << 6)
/* RQR */
#define USART_RQR_RXFRQ (1u << 3)

/* 80 MHz PCLK2: 9600 -> USARTDIV = 8333.33 (mant 520, frac 13 = 0x208D);
   115200 -> 694.44 (mant 43, frac 7 = 0x2B7). */
#define USART_BRR_9600 0x208Du
#define USART_BRR_115200 0x02B7u

#define USART1_IRQN 37

/* ---- core peripherals ---- */
#define NVIC_ISER0 (*(volatile uint32_t*)0xE000E100u)
#define NVIC_ISER1 (*(volatile uint32_t*)0xE000E104u)  /* IRQ 32..63 */
#define SCB_CPACR (*(volatile uint32_t*)0xE000ED88u)
#define SYST_CSR (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018u)

static inline void __nop(void) { __asm volatile("nop"); }

#endif /* __MAIN_H */