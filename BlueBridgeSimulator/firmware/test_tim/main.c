// CT117E-M4 TIM test firmware (bare metal, no HAL).
//
// Drives every timer feature through real memory-mapped registers so the
// simulator can verify the whole chain:
//
//   firmware -> TIMx registers -> counter/PWM -> AF router -> GPIO pin
//   external generator -> GPIO pin -> AF router -> TIMx capture -> firmware
//
// Test phases (selected by the host through the mailbox command word):
//   0  idle          -- all timers stopped
//   1  base timer    -- TIM3 PSC=79 ARR=999, 1 kHz update interrupt only
//   2  PWM 50 %      -- TIM3_CH2 -> PA7 (AF2), CCR2 = 500
//   3  PWM 25 %      -- same, CCR2 = 250
//   4  PWM 75 %      -- same, CCR2 = 750
//   5  input capture -- TIM2_CH1 on PA15 (AF1), 1 MHz counter, rising edge
//   6  wrong AF      -- PWM configured but PA7 left as a plain input
//   7  CEN = 0       -- PWM configured but the counter is never started
//   8  PWM 2 kHz     -- same wiring, ARR = 499 (frequency change)
//   9  PWM 10 kHz    -- ARR = 7999 (edge density / performance)
//   10 PWM 20 kHz    -- ARR = 3999 (edge density / performance)
//
// Mailbox at 0x20000000 (host readable/writable, see tests/tim_selftest.cpp):
//   [0]  magic            0xC0FFEE03
//   [1]  cmd              written by the host: requested phase
//   [2]  phase            applied phase (the firmware mirrors cmd)
//   [3]  tim3UpdateIrq    TIM3 update interrupt count
//   [4]  tim2CaptureIrq   TIM2 capture interrupt count
//   [5]  lastCapture      CCR1 value latched by the last capture
//   [6]  captureDelta     counter ticks between the last two captures
//                         (overflow corrected by the firmware itself)
//   [7]  pwmCcr           current TIM3->CCR2 (duty)
//   [8]  tim3Cnt          TIM3->CNT snapshot
//   [9]  tim3Sr           TIM3->SR snapshot
//   [10] tim2Cnt          TIM2->CNT snapshot
//   [11] loopCount        main loop iterations
//   [12] tim2OverflowIrq  TIM2 counter overflow count
#include <stdint.h>

// ---- mailbox (linked at 0x20000000, initialized) ----
__attribute__((section(".selfdata"), used))
volatile uint32_t g_mailbox[13] = {
    0xC0FFEE03u,  // magic
    0,            // cmd (host -> firmware)
    0,            // phase (firmware -> host)
    0,            // TIM3 update IRQ count
    0,            // TIM2 capture IRQ count
    0,            // last capture value
    0,            // capture delta (counter ticks between two rising edges)
    0,            // PWM CCR2
    0,            // TIM3 CNT
    0,            // TIM3 SR
    0,            // TIM2 CNT
    0,            // loop count
    0,            // TIM2 overflow count
};

#define MAIL_CMD    (g_mailbox[1])
#define MAIL_PHASE  (g_mailbox[2])
#define MAIL_UPD    (g_mailbox[3])
#define MAIL_CAP    (g_mailbox[4])
#define MAIL_LASTCAP (g_mailbox[5])
#define MAIL_DELTA  (g_mailbox[6])
#define MAIL_CCR2   (g_mailbox[7])
#define MAIL_CNT3   (g_mailbox[8])
#define MAIL_SR3    (g_mailbox[9])
#define MAIL_CNT2   (g_mailbox[10])
#define MAIL_LOOP   (g_mailbox[11])
#define MAIL_OVF    (g_mailbox[12])

// ---- register definitions (STM32G4, verified against RM0440/CMSIS) ----
#define RCC_BASE 0x40021000u
#define RCC_CR       (*(volatile uint32_t*)(RCC_BASE + 0x00u))
#define RCC_CFGR     (*(volatile uint32_t*)(RCC_BASE + 0x08u))
#define RCC_PLLCFGR  (*(volatile uint32_t*)(RCC_BASE + 0x0Cu))
#define RCC_APB1ENR  (*(volatile uint32_t*)(RCC_BASE + 0x58u))
#define RCC_APB2ENR  (*(volatile uint32_t*)(RCC_BASE + 0x60u))
#define RCC_AHB2ENR  (*(volatile uint32_t*)(RCC_BASE + 0x4Cu))

#define FLASH_ACR    (*(volatile uint32_t*)0x40022000u)

#define GPIOA_BASE 0x48000000u
#define GPIO_MODER(g) (*(volatile uint32_t*)(g + 0x00u))
#define GPIO_PUPDR(g) (*(volatile uint32_t*)(g + 0x0Cu))
#define GPIO_IDR(g)   (*(volatile uint32_t*)(g + 0x10u))
#define GPIO_ODR(g)   (*(volatile uint32_t*)(g + 0x14u))
#define GPIO_BSRR(g)  (*(volatile uint32_t*)(g + 0x18u))
#define GPIO_AFRL(g)  (*(volatile uint32_t*)(g + 0x20u))
#define GPIO_AFRH(g)  (*(volatile uint32_t*)(g + 0x24u))

// TIM2 (APB1, 32-bit) / TIM3 (APB1): RM0440 register map
#define TIM2_BASE 0x40000000u
#define TIM3_BASE 0x40000400u
#define TIM_CR1(t)   (*(volatile uint32_t*)((t) + 0x00u))
#define TIM_DIER(t)  (*(volatile uint32_t*)((t) + 0x0Cu))
#define TIM_SR(t)    (*(volatile uint32_t*)((t) + 0x10u))
#define TIM_EGR(t)   (*(volatile uint32_t*)((t) + 0x14u))
#define TIM_CCMR1(t) (*(volatile uint32_t*)((t) + 0x18u))
#define TIM_CCER(t)  (*(volatile uint32_t*)((t) + 0x20u))
#define TIM_CNT(t)   (*(volatile uint32_t*)((t) + 0x24u))
#define TIM_PSC(t)   (*(volatile uint32_t*)((t) + 0x28u))
#define TIM_ARR(t)   (*(volatile uint32_t*)((t) + 0x2Cu))
#define TIM_CCR1(t)  (*(volatile uint32_t*)((t) + 0x34u))
#define TIM_CCR2(t)  (*(volatile uint32_t*)((t) + 0x38u))

#define NVIC_ISER0 (*(volatile uint32_t*)0xE000E100u)

#define SCB_CPACR (*(volatile uint32_t*)0xE000ED88u)

#define TIM_IRQ_TIM2 28
#define TIM_IRQ_TIM3 29

// ---------------------------------------------------------------------------
// interrupt handlers (real Cortex-M exceptions, entered by the simulator)
// ---------------------------------------------------------------------------
static volatile uint32_t s_prevCapture;
static volatile uint32_t s_prevOvf;
static volatile uint32_t s_havePrevCapture;
static volatile uint32_t s_ovfCount;

void TIM3_IRQHandler(void) {
    // update event: clear UIF (write 0 to clear) and count
    TIM_SR(TIM3_BASE) = ~1u;
    MAIL_UPD++;
}

void TIM2_IRQHandler(void) {
    // One IRQ line covers both the capture and the overflow event: read the
    // status register once, handle what is pending, then clear those flags.
    const uint32_t sr = TIM_SR(TIM2_BASE);
    uint32_t handled = 0;

    if (sr & 1u) {  // UIF: counter wrapped (16-bit counter, ARR = 0xFFFF)
        s_ovfCount++;
        MAIL_OVF = s_ovfCount;
        handled |= 1u;
    }
    if (sr & 2u) {  // CC1IF: capture event
        const uint32_t cap = TIM_CCR1(TIM2_BASE);  // latched CNT value
        MAIL_LASTCAP = cap;
        if (s_havePrevCapture) {
            // The firmware does its own overflow handling (the simulator must
            // not "fix" the capture value): CCRx really holds the raw CNT at
            // the edge, so the period has to be rebuilt from the wrap count.
            MAIL_DELTA = (s_ovfCount - s_prevOvf) * 65536u +
                         (cap - s_prevCapture);
        }
        s_prevCapture = cap;
        s_prevOvf = s_ovfCount;
        s_havePrevCapture = 1;
        MAIL_CAP++;
        handled |= 2u;
    }
    if (handled) TIM_SR(TIM2_BASE) = ~handled;
}

// ---------------------------------------------------------------------------
// clock tree: HSE 24 MHz -> PLL -> 80 MHz (same as test_blink)
// ---------------------------------------------------------------------------
static void clock_init(void) {
    RCC_CR |= (1u << 16);
    while ((RCC_CR & (1u << 17)) == 0) {}

    RCC_PLLCFGR = (3u << 0) | (2u << 4) | (20u << 8) | (1u << 24);
    RCC_CR |= (1u << 24);
    while ((RCC_CR & (1u << 25)) == 0) {}

    FLASH_ACR = 2u;

    RCC_CFGR = (RCC_CFGR & ~3u) | 3u;
    while (((RCC_CFGR >> 2) & 3u) != 3u) {}
}

static void gpio_init(void) {
    RCC_AHB2ENR |= 0x1Fu;  // GPIOA..E
    (void)RCC_AHB2ENR;
    (void)RCC_AHB2ENR;

    // PA7 / PA15 start as plain inputs (analog on real reset): the AF
    // configuration is done per test phase so "wrong AF" can be provoked.
    GPIO_MODER(GPIOA_BASE) &= ~((3u << (7 * 2)) | (3u << (15 * 2)));
    GPIO_PUPDR(GPIOA_BASE) &= ~((3u << (7 * 2)) | (3u << (15 * 2)));
}

// PA7 -> TIM3_CH2 (AF2), PA15 -> TIM2_CH1 (AF1)
static void pa7_enable_af2(void) {
    // MODER = alternate function
    GPIO_MODER(GPIOA_BASE) =
        (GPIO_MODER(GPIOA_BASE) & ~(3u << (7 * 2))) | (2u << (7 * 2));
    // PA7 lives in AFRL (pins 0..7), AF number 2
    GPIO_AFRL(GPIOA_BASE) = (GPIO_AFRL(GPIOA_BASE) & ~(0xFu << (7 * 4))) |
                            (2u << (7 * 4));
}

static void pa15_enable_af1(void) {
    GPIO_MODER(GPIOA_BASE) =
        (GPIO_MODER(GPIOA_BASE) & ~(3u << (15 * 2))) | (2u << (15 * 2));
    // AFRH: PA15 -> AF1 (bits 28..31 of AFRH)
    GPIO_AFRH(GPIOA_BASE) = (GPIO_AFRH(GPIOA_BASE) & ~(0xFu << ((15 - 8) * 4))) |
                            (1u << ((15 - 8) * 4));
}

static void pa7_disable_af(void) {
    GPIO_MODER(GPIOA_BASE) &= ~(3u << (7 * 2));  // input, no AF
}

// ---------------------------------------------------------------------------
// timer setups (exactly what a HAL/LL init sequence would program)
// ---------------------------------------------------------------------------
static void tim3_stop(void) {
    TIM_CR1(TIM3_BASE) = 0;
    TIM_DIER(TIM3_BASE) = 0;
    TIM_CCER(TIM3_BASE) = 0;
    TIM_CCMR1(TIM3_BASE) = 0;
    TIM_SR(TIM3_BASE) = 0;
    TIM_CNT(TIM3_BASE) = 0;
}

static void tim2_stop(void) {
    TIM_CR1(TIM2_BASE) = 0;
    TIM_DIER(TIM2_BASE) = 0;
    TIM_CCER(TIM2_BASE) = 0;
    TIM_CCMR1(TIM2_BASE) = 0;
    TIM_SR(TIM2_BASE) = 0;
    TIM_CNT(TIM2_BASE) = 0;
}

// TIM3 update interrupt at 1 kHz (80 MHz / 80 = 1 MHz counter, / 1000 = 1 kHz)
static void tim3_base_timer(void) {
    tim3_stop();
    TIM_PSC(TIM3_BASE) = 79u;
    TIM_ARR(TIM3_BASE) = 999u;
    TIM_DIER(TIM3_BASE) = 0x0001u;  // UIE
    TIM_EGR(TIM3_BASE) = 0x0001u;   // UG: reload PSC/ARR, clear CNT + UIF
    TIM_SR(TIM3_BASE) = 0;
    NVIC_ISER0 = 1u << TIM_IRQ_TIM3;
    TIM_CR1(TIM3_BASE) = 0x0001u;   // CEN = 1
}

// TIM3_CH2 PWM on PA7, duty = ccr/(arr+1)
static void tim3_pwm(uint32_t psc, uint32_t arr, uint32_t ccr, int configure_af,
                     int enable_counter) {
    tim3_stop();
    if (configure_af)
        pa7_enable_af2();
    else
        pa7_disable_af();

    TIM_PSC(TIM3_BASE) = psc;
    TIM_ARR(TIM3_BASE) = arr;
    // OC2M = 110 (PWM mode 1), OC2PE = 1 (preload enable) -> channel 2 byte
    TIM_CCMR1(TIM3_BASE) = (0x6u << 4 | 0x8u) << 8;
    TIM_CCR2(TIM3_BASE) = ccr;
    TIM_CCER(TIM3_BASE) = 0x0010u;          // CC2E = 1, CC2P = 0 (active high)
    TIM_DIER(TIM3_BASE) = 0x0001u;          // UIE (phase/update counting)
    TIM_EGR(TIM3_BASE) = 0x0001u;           // UG: load PSC/ARR/CCR preloads
    TIM_SR(TIM3_BASE) = 0;
    NVIC_ISER0 = 1u << TIM_IRQ_TIM3;
    TIM_CR1(TIM3_BASE) = enable_counter ? 0x0001u : 0x0000u;
}

// TIM2_CH1 input capture on PA15, 1 MHz counter, rising edge, CC1 interrupt
static void tim2_capture(void) {
    tim2_stop();
    pa15_enable_af1();

    TIM_PSC(TIM2_BASE) = 79u;         // 1 MHz counter clock
    TIM_ARR(TIM2_BASE) = 0xFFFFu;     // full range: overflow is handled by the ISR
    TIM_CCMR1(TIM2_BASE) = 0x0001u;   // CC1S = 01: input, IC1 mapped on TI1
    TIM_CCER(TIM2_BASE) = 0x0001u;    // CC1E = 1, CC1P = 0 (rising edge)
    TIM_DIER(TIM2_BASE) = 0x0003u;    // UIE (overflow counting) + CC1IE
    TIM_EGR(TIM2_BASE) = 0x0001u;     // UG: reload PSC/ARR, clear CNT
    TIM_SR(TIM2_BASE) = 0;
    s_prevCapture = 0;
    s_prevOvf = 0;
    s_ovfCount = 0;
    s_havePrevCapture = 0;
    MAIL_DELTA = 0;
    MAIL_OVF = 0;
    NVIC_ISER0 = 1u << TIM_IRQ_TIM2;
    TIM_CR1(TIM2_BASE) = 0x0001u;     // CEN = 1
}

static void apply_phase(uint32_t phase) {
    switch (phase) {
    case 1:  // base timer interrupt
        tim2_stop();
        tim3_base_timer();
        break;
    case 2:
        tim3_pwm(79u, 999u, 500u, /*af=*/1, /*cen=*/1);
        break;
    case 3:
        tim3_pwm(79u, 999u, 250u, /*af=*/1, /*cen=*/1);
        break;
    case 4:
        tim3_pwm(79u, 999u, 750u, /*af=*/1, /*cen=*/1);
        break;
    case 5:  // input capture
        tim3_stop();
        tim2_capture();
        break;
    case 6:  // PWM configured, AF deliberately NOT configured
        tim3_pwm(79u, 999u, 500u, /*af=*/0, /*cen=*/1);
        break;
    case 7:  // PWM configured, counter never started (CEN = 0)
        tim3_pwm(79u, 999u, 500u, /*af=*/1, /*cen=*/0);
        break;
    case 8:  // PWM 2 kHz 50 % (ARR change -> frequency change)
        tim3_pwm(79u, 499u, 250u, /*af=*/1, /*cen=*/1);
        break;
    case 9:  // PWM 10 kHz 50 % (PSC = 0 -> 80 MHz counter)
        tim3_pwm(0u, 7999u, 4000u, /*af=*/1, /*cen=*/1);
        break;
    case 10:  // PWM 20 kHz 50 %
        tim3_pwm(0u, 3999u, 2000u, /*af=*/1, /*cen=*/1);
        break;
    default:  // idle
        tim3_stop();
        tim2_stop();
        break;
    }
    MAIL_PHASE = phase;
}

int main(void) {
    SCB_CPACR = (0xFu << 20);
    __asm volatile("dsb");
    __asm volatile("isb");

    clock_init();
    gpio_init();

    RCC_APB1ENR |= (1u << 0) | (1u << 1);  // TIM2EN, TIM3EN (real firmware does)
    (void)RCC_APB1ENR;

    apply_phase(0);

    for (;;) {
        MAIL_LOOP++;
        const uint32_t cmd = MAIL_CMD;
        if (cmd != MAIL_PHASE) apply_phase(cmd);

        MAIL_CCR2 = TIM_CCR2(TIM3_BASE);
        MAIL_CNT3 = TIM_CNT(TIM3_BASE);
        MAIL_SR3 = TIM_SR(TIM3_BASE);
        MAIL_CNT2 = TIM_CNT(TIM2_BASE);
    }
}