// CT117E-M4 TIM demo firmware (bare metal + official LCD BSP).
//
// What it demonstrates, all through real memory-mapped registers:
//   1. TIM2 update interrupt every 0.5 s  -> LD1 blinks (0.5 s on / 0.5 s off)
//   2. TIM3_CH2 PWM  1 kHz / 75 %         -> PA7 (AF2), the board pin
//   3. LCD (ILI9325, official BSP)        -> shows "TIM test" + PWM/LED info
//
//   firmware -> TIMx registers -> counter/compare -> AF router -> PA7 pin
//   TIMx update event -> SR.UIF -> NVIC -> real Cortex-M ISR -> LED latch
//
// Clocking (80 MHz SYSCLK, APB1 prescaler = 1):
//   TIM2/TIM3 timer clock = 80 MHz
//   TIM2: PSC = 7999 -> 10 kHz,  ARR = 4999 -> 0.5 s update interrupt
//   TIM3: PSC = 79   -> 1 MHz,   ARR = 999  -> 1 kHz PWM, CCR2 = 750 -> 75 %
//
// Mailbox at 0x20000000 (.selfdata) for headless verification:
//   [0] magic 0x71D00001   [1] uwTick (ms)      [2] main loop count
//   [3] TIM2 IRQ count     [4] LED pattern      [5] uptime seconds
//   [6] TIM3->CCR2         [7] TIM3->CR1 (CEN)
#include "lcd.h"  // official CT117E LCD BSP (brings in main.h)

__attribute__((section(".selfdata"), used))
volatile uint32_t g_mailbox[8] = {
    0x71D00001u,  // magic
    0,            // uwTick (ms)
    0,            // main loop count
    0,            // TIM2 update interrupt count (0.5 s each)
    0,            // LED pattern (bit i = LD(i+1) on)
    0,            // uptime seconds
    0,            // TIM3->CCR2 (duty)
    0,            // TIM3->CR1 (CEN bit)
};

#define MAIL_TICK  (g_mailbox[1])
#define MAIL_LOOP  (g_mailbox[2])
#define MAIL_BLINK (g_mailbox[3])
#define MAIL_LED   (g_mailbox[4])
#define MAIL_SEC   (g_mailbox[5])
#define MAIL_CCR2  (g_mailbox[6])
#define MAIL_CR1   (g_mailbox[7])

// ---- registers (STM32G431, RM0440 / CMSIS) ----
#define RCC_BASE 0x40021000u
#define RCC_CR (*(volatile uint32_t*)(RCC_BASE + 0x00u))
#define RCC_CFGR (*(volatile uint32_t*)(RCC_BASE + 0x08u))
#define RCC_PLLCFGR (*(volatile uint32_t*)(RCC_BASE + 0x0Cu))
#define RCC_AHB2ENR (*(volatile uint32_t*)(RCC_BASE + 0x4Cu))
#define RCC_APB1ENR (*(volatile uint32_t*)(RCC_BASE + 0x58u))

#define FLASH_ACR (*(volatile uint32_t*)0x40022000u)

#define TIM2 ((volatile uint32_t*)0x40000000u)
#define TIM3 ((volatile uint32_t*)0x40000400u)
#define TIM_CR1(t) (t[0x00u / 4])
#define TIM_DIER(t) (t[0x0Cu / 4])
#define TIM_SR(t) (t[0x10u / 4])
#define TIM_EGR(t) (t[0x14u / 4])
#define TIM_CCMR1(t) (t[0x18u / 4])
#define TIM_CCER(t) (t[0x20u / 4])
#define TIM_PSC(t) (t[0x28u / 4])
#define TIM_ARR(t) (t[0x2Cu / 4])
#define TIM_CCR2(t) (t[0x38u / 4])

#define NVIC_ISER0 (*(volatile uint32_t*)0xE000E100u)
#define SCB_CPACR (*(volatile uint32_t*)0xE000ED88u)

#define SYST_CSR (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018u)

#define TIM_IRQ_TIM2 28
#define TIM3EN_BIT 1u
#define TIM2EN_BIT 0u

static volatile uint32_t g_blinkCount = 0;
static volatile uint32_t g_ledPattern = 0;

// ---------------------------------------------------------------------------
// 74LS573 LED bank: active low on PC8..PC15, latch enable = PD2
// ---------------------------------------------------------------------------
static void led_disp(uint32_t pattern) {
    uint32_t odr = GPIOC->ODR & 0x00FFu;             // keep the LCD data low byte
    odr |= (~pattern & 0xFFu) << 8;
    GPIOC->ODR = odr;
    GPIOD->BSRR = (1u << 2);                         // LE = 1 (transparent)
    GPIOD->BSRR = (1u << 2) << 16;                   // LE = 0 (hold)
}

// ---------------------------------------------------------------------------
// 0.5 s blink: real TIM2 update interrupt
// ---------------------------------------------------------------------------
void TIM2_IRQHandler(void) {
    TIM_SR(TIM2) = ~1u;      // SR is cleared by writing 0 (RM0440)
    g_blinkCount++;
    g_ledPattern ^= 0x01u;   // LD1: 0.5 s on / 0.5 s off
    led_disp(g_ledPattern);
}

void SysTick_Handler(void) { g_uwTick++; }

// ---------------------------------------------------------------------------
// clock / time base
// ---------------------------------------------------------------------------
static void clock_init(void) {
    RCC_CR |= (1u << 16);                              // HSE on (24 MHz)
    while ((RCC_CR & (1u << 17)) == 0) {}

    RCC_PLLCFGR = (3u << 0) | (2u << 4) | (20u << 8) | (1u << 24);
    RCC_CR |= (1u << 24);                              // PLL on
    while ((RCC_CR & (1u << 25)) == 0) {}

    FLASH_ACR = 2u;                                    // 2 wait states
    RCC_CFGR = (RCC_CFGR & ~3u) | 3u;                  // SYSCLK <- PLL 80 MHz
    while (((RCC_CFGR >> 2) & 3u) != 3u) {}
}

static void systick_init(void) {
    SYST_RVR = 79999u;   // 1 kHz
    SYST_CVR = 0;
    SYST_CSR = 7u;       // ENABLE | TICKINT | CLKSOURCE
}

// ---------------------------------------------------------------------------
// PWM: PA7 = TIM3_CH2 (AF2), 1 kHz, 75 %
// ---------------------------------------------------------------------------
static void pwm_init(void) {
    // PA7 in alternate function mode, AF2 = TIM3_CH2 (RM0440 AFRL)
    GPIOA->MODER = (GPIOA->MODER & ~(3u << (7 * 2))) | (2u << (7 * 2));
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(0xFu << (7 * 4))) | (2u << (7 * 4));

    RCC_APB1ENR |= (1u << TIM3EN_BIT);   // TIM3 clock enable (real firmware does)

    TIM_CR1(TIM3) = 0;                   // stop while configuring
    TIM_PSC(TIM3) = 79u;                 // 80 MHz / 80  = 1 MHz counter
    TIM_ARR(TIM3) = 999u;                // 1 MHz / 1000 = 1 kHz period
    TIM_CCR2(TIM3) = 750u;               // 750/1000 = 75 %
    // OC2M = 110 (PWM mode 1), OC2PE = 1 (CCR2 preload) -> channel 2 byte
    TIM_CCMR1(TIM3) = ((0x6u << 4) | 0x8u) << 8;
    TIM_CCER(TIM3) = 0x0010u;            // CC2E = 1, active high
    TIM_EGR(TIM3) = 1u;                  // UG: load PSC/ARR/CCR preloads
    TIM_SR(TIM3) = 0;
    TIM_CR1(TIM3) = 1u;                  // CEN = 1
}

// ---------------------------------------------------------------------------
// blink timer: TIM2 update interrupt every 0.5 s
// ---------------------------------------------------------------------------
static void blink_timer_init(void) {
    RCC_APB1ENR |= (1u << TIM2EN_BIT);   // TIM2 clock enable

    TIM_CR1(TIM2) = 0;
    TIM_PSC(TIM2) = 7999u;               // 80 MHz / 8000 = 10 kHz counter
    TIM_ARR(TIM2) = 4999u;               // 10 kHz / 5000 = 2 Hz -> 0.5 s
    TIM_DIER(TIM2) = 1u;                 // UIE
    TIM_EGR(TIM2) = 1u;                  // UG: reload PSC/ARR, clear CNT + UIF
    TIM_SR(TIM2) = 0;
    NVIC_ISER0 = 1u << TIM_IRQ_TIM2;     // enable TIM2 in the NVIC
    TIM_CR1(TIM2) = 1u;                  // CEN = 1
}

// ---------------------------------------------------------------------------
// LCD text (official BSP). All LCD traffic happens either before the TIM2
// interrupt is enabled or with interrupts masked, because the LED latch and
// the LCD data bus share PC8..PC15 -- exactly the hazard a real board has.
// ---------------------------------------------------------------------------
static void lcd_show_static(void) {
    LCD_Init();                          // controller detection + init over the bus
    LCD_Clear(Black);
    LCD_SetBackColor(Black);

    LCD_SetTextColor(Green);
    LCD_DisplayStringLine(Line1, (u8*)"TIM test");
    LCD_SetTextColor(White);
    LCD_DisplayStringLine(Line3, (u8*)"PWM 1kHz 75% PA7");
    LCD_DisplayStringLine(Line4, (u8*)"TIM3_CH2 AF2");
    LCD_DisplayStringLine(Line5, (u8*)"LED 0.5s TIM2 IRQ");
    LCD_SetTextColor(Yellow);
    LCD_DisplayStringLine(Line7, (u8*)"RUN: 0 s");
}

// "RUN: n s" without libc
static void lcd_show_seconds(uint32_t sec) {
    u8 buf[16];
    int n = 0;
    const u8* prefix = (u8*)"RUN: ";
    for (const u8* p = prefix; *p; p++) buf[n++] = *p;
    char digits[12];
    int d = 0;
    if (sec == 0) {
        digits[d++] = '0';
    } else {
        while (sec > 0 && d < 12) {
            digits[d++] = (char)('0' + (sec % 10u));
            sec /= 10u;
        }
    }
    while (d > 0) buf[n++] = (u8)digits[--d];
    buf[n++] = ' ';
    buf[n++] = 's';
    buf[n++] = 0;
    LCD_DisplayStringLine(Line7, buf);
}

int main(void) {
    SCB_CPACR = (0xFu << 20);  // FPU on (the BSP/font code is hard-float)
    __asm volatile("dsb");
    __asm volatile("isb");

    clock_init();
    systick_init();

    // GPIOD peripheral clock FIRST: the 74LS573 latch enable is PD2 and, like
    // on the real part, writes to an unclocked GPIO port are dropped (RM0440
    // RCC_AHB2ENR). Without this the latch never opens and the LEDs stay dark
    // even though everything else (timers, LCD, PWM) works.
    RCC_AHB2ENR |= (1u << 3);   // GPIODEN
    (void)RCC_AHB2ENR;
    (void)RCC_AHB2ENR;

    // PD2 = 74LS573 latch enable (LED bank)
    GPIOD->MODER = (GPIOD->MODER & ~(3u << (2 * 2))) | (1u << (2 * 2));
    GPIOD->BSRR = (1u << 2) << 16;  // LE = 0
    led_disp(0);                    // all LEDs off

    lcd_show_static();              // LCD content first (no interrupt running yet)

    pwm_init();                     // PA7: 1 kHz / 75 %
    MAIL_CCR2 = TIM_CCR2(TIM3);
    MAIL_CR1 = TIM_CR1(TIM3);

    blink_timer_init();             // now the 0.5 s interrupts start

    uint32_t lastSec = 0;
    for (;;) {
        MAIL_LOOP++;
        MAIL_TICK = g_uwTick;
        MAIL_LED = g_ledPattern;   // always current, so a readback sees the blink

        const uint32_t sec = g_uwTick / 1000u;
        if (sec != lastSec) {
            lastSec = sec;
            MAIL_SEC = sec;
            MAIL_BLINK = g_blinkCount;
            MAIL_CCR2 = TIM_CCR2(TIM3);
            MAIL_CR1 = TIM_CR1(TIM3);

            // The ISR also touches GPIOC (LED latch) -> mask it while the LCD
            // data bus is being driven, otherwise a comparison could mix.
            __asm volatile("cpsid i");
            lcd_show_seconds(sec);
            __asm volatile("cpsie i");
        }
    }
}