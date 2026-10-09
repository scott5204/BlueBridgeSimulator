// CT117E-M4 self-test firmware (bare metal, no HAL).
//
// Exercises exactly the stage-1 simulator feature set through real
// memory-mapped registers:
//   - RCC: HSE on -> PLL (24MHz /3 *20 /2 = 80MHz) -> SYSCLK switch
//   - FLASH ACR latency, GPIO clocks (AHB2ENR)
//   - GPIO: PC8-15 output (LED data), PD2 output (74LS573 LE),
//           PB0-2 + PA0 inputs with pull-ups (buttons B1-B4)
//   - SysTick: 1 kHz interrupt, uwTick counter (mailbox)
//   - FPU: CPACR enable + float math (hard-float, VFPv4)
//
// Observable behaviour:
//   - walking LED pattern on LD1..LD8 every 250 ms (60 ms while B4 held)
//   - B1 held: all LEDs on
//   - mailbox at 0x20000000: [0]=magic [1]=uwTick [2]=loopCount
//                             [3]=mode [4]=float accumulator bits
#include <stdint.h>

// ---- mailbox (linked at 0x20000000, initialized) ----
__attribute__((section(".selfdata"), used))
volatile uint32_t g_mailbox[5] = {
    0xC0FFEE01u,  // magic
    0,            // uwTick (ms)
    0,            // main loop count
    0,            // mode: 0=boot 1=pattern 2=B1-held
    0,            // float accumulator raw bits
};

#define MAIL_TICK  (g_mailbox[1])
#define MAIL_LOOP  (g_mailbox[2])
#define MAIL_MODE  (g_mailbox[3])
#define MAIL_FLOAT (g_mailbox[4])

// ---- register definitions (STM32G4, verified against RM0440/CMSIS) ----
#define RCC_BASE 0x40021000u
#define RCC_CR       (*(volatile uint32_t*)(RCC_BASE + 0x00u))
#define RCC_CFGR     (*(volatile uint32_t*)(RCC_BASE + 0x08u))
#define RCC_PLLCFGR  (*(volatile uint32_t*)(RCC_BASE + 0x0Cu))
#define RCC_AHB2ENR  (*(volatile uint32_t*)(RCC_BASE + 0x4Cu))

#define FLASH_ACR    (*(volatile uint32_t*)0x40022000u)

#define GPIOA_BASE 0x48000000u
#define GPIOB_BASE 0x48000400u
#define GPIOC_BASE 0x48000800u
#define GPIOD_BASE 0x48000C00u
#define GPIO_MODER(g) (*(volatile uint32_t*)(g + 0x00u))
#define GPIO_PUPDR(g) (*(volatile uint32_t*)(g + 0x0Cu))
#define GPIO_IDR(g)   (*(volatile uint32_t*)(g + 0x10u))
#define GPIO_ODR(g)   (*(volatile uint32_t*)(g + 0x14u))
#define GPIO_BSRR(g)  (*(volatile uint32_t*)(g + 0x18u))

#define SYST_CSR (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018u)

#define SCB_CPACR (*(volatile uint32_t*)0xE000ED88u)

static inline void led_latch_pulse(void) {
    GPIO_BSRR(GPIOD_BASE) = (1u << 2);        // PD2 = 1 (latch open)
    GPIO_BSRR(GPIOD_BASE) = (1u << 2) << 16;  // PD2 = 0 (latch hold)
}

// Write the LED bank: bit i = LD(i+1) on. Active low on PC8..PC15.
static void led_disp(uint8_t v) {
    uint32_t odr = GPIO_ODR(GPIOC_BASE) & 0x00FFu;
    odr |= (uint32_t)(~v & 0xFFu) << 8;
    GPIO_ODR(GPIOC_BASE) = odr;
    led_latch_pulse();
}

void SysTick_Handler(void) {
    MAIL_TICK++;
}

static void clock_init(void) {
    // HSE on (24 MHz crystal on CT117E-M4)
    RCC_CR |= (1u << 16);
    while ((RCC_CR & (1u << 17)) == 0) {}

    // PLL: HSE /M=3 *N=20 /R=2 -> 80 MHz
    // encodings (RM0440 / HAL): PLLSRC=11b(HSE), M=(3-1)<<4, N=20<<8,
    //                           R field 00b = /2, PLLREN bit24
    RCC_PLLCFGR = (3u << 0) | (2u << 4) | (20u << 8) | (1u << 24);
    RCC_CR |= (1u << 24);
    while ((RCC_CR & (1u << 25)) == 0) {}

    // flash latency 2 WS (not modeled in the simulator, harmless)
    FLASH_ACR = 2u;

    // SYSCLK <- PLL
    RCC_CFGR = (RCC_CFGR & ~3u) | 3u;
    while (((RCC_CFGR >> 2) & 3u) != 3u) {}
}

static void gpio_init(void) {
    RCC_AHB2ENR |= 0x1Fu;  // GPIOA..E clocks
    (void)RCC_AHB2ENR;
    (void)RCC_AHB2ENR;

    // PC8..PC15: push-pull outputs, LEDs off (active low)
    GPIO_MODER(GPIOC_BASE) =
        (GPIO_MODER(GPIOC_BASE) & 0x0000FFFFu) | 0x55550000u;
    GPIO_ODR(GPIOC_BASE) |= 0xFF00u;

    // PD2: output (74LS573 LE)
    GPIO_MODER(GPIOD_BASE) = (GPIO_MODER(GPIOD_BASE) & ~0x30u) | 0x10u;
    GPIO_ODR(GPIOD_BASE) &= ~(1u << 2);

    // buttons: PB0..PB2 (B1..B3), PA0 (B4), inputs with pull-ups
    GPIO_MODER(GPIOB_BASE) &= ~0x3Fu;
    GPIO_PUPDR(GPIOB_BASE) = (GPIO_PUPDR(GPIOB_BASE) & ~0x3Fu) | 0x15u;
    GPIO_MODER(GPIOA_BASE) &= ~0x3u;
    GPIO_PUPDR(GPIOA_BASE) = (GPIO_PUPDR(GPIOA_BASE) & ~0x3u) | 0x1u;
}

static void systick_init(void) {
    SYST_RVR = 79999u;  // 80 MHz / 1000 Hz - 1
    SYST_CVR = 0;
    SYST_CSR = 7u;       // ENABLE | TICKINT | CLKSOURCE(processor clock)
}

int main(void) {
    // FPU enable (CP10/CP11 full access) - exercises the CPACR path
    SCB_CPACR = (0xFu << 20);
    __asm volatile("dsb");
    __asm volatile("isb");

    clock_init();
    gpio_init();
    systick_init();

    led_disp(0);

    uint8_t pos = 0;
    uint32_t last = 0;
    union {
        float f;
        uint32_t u;
    } acc = {0.0f};

    for (;;) {
        MAIL_LOOP++;

        const uint32_t now = MAIL_TICK;
        const int b1 = ((GPIO_IDR(GPIOB_BASE) >> 0) & 1u) == 0;  // B1 PB0
        const int b4 = ((GPIO_IDR(GPIOA_BASE) >> 0) & 1u) == 0;  // B4 PA0

        if (b1) {
            MAIL_MODE = 2;
            led_disp(0xFF);  // B1 held: all LEDs on
            last = now;
        } else {
            MAIL_MODE = 1;
            const uint32_t period = b4 ? 60u : 250u;  // B4: fast blink
            if ((now - last) >= period) {
                last = now;
                pos = (pos + 1u) & 7u;
                led_disp((uint8_t)(1u << pos));

                // FPU work: hard-float VFPv4 instructions
                acc.f += 0.5f;
                if (acc.f > 64.0f) acc.f -= 64.0f;
                MAIL_FLOAT = acc.u;
            }
        }
    }
}
