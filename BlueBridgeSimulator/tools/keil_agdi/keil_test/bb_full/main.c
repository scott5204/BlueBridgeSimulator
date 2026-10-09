// CT117E-M4 "SELF TEST" firmware -- ONE program to exercise everything the
// simulator models, for manual GUI testing:
//
//   1) LED chaser        PC8..PC15 through the 74LS573 latch (PD2)
//   2) Screen test       B4 switches pages: status / LCD geometry / LED patterns
//   3) Key test          B1..B4, 20 ms debounce, per-key press counters
//   4) Signal output     PA7 = TIM3_CH2 PWM, presets cycled by B3
//   5) Signal input      PA15 = TIM2_CH1 capture, PB4 = TIM16_CH1 capture
//   6) Analog input      R37 (ADC2_IN15) / R38 (ADC1_IN11) shown on the LCD
//
// Key map:  B1 = LED direction   B2 = LED speed (400/200/100/50 ms)
//           B3 = PWM preset      B4 = next LCD page
//
// Everything is plain memory-mapped I/O (no HAL) so the simulator's peripheral
// models are exercised exactly as a competition firmware would.
//
// Mailbox at 0x20000000 (.selfdata); a headless test can write [1] to switch
// the page, and read the rest to check the result:
//   [0]  magic 0xC0FFEE05      [1] cmd (host -> firmware: page to show)
//   [2]  page                  [3] led pattern
//   [4]  key press counts (B1<<0, B2<<8, B3<<16, B4<<24)
//   [5]  key currently down (bit0..bit3)
//   [6]  led direction         [7] led speed (ms per step)
//   [8]  PA15 measured Hz      [9] PB4 measured Hz
//   [10] PWM preset index      [11] PA7 configured Hz   [12] duty permille
//   [13] R37 millivolts        [14] R38 millivolts
//   [15] main loop count       [16] LCD full redraws
#include "lcd.h"  // official CT117E LCD BSP (brings in main.h)

__attribute__((section(".selfdata"), used))
volatile uint32_t g_mailbox[17] = {
    0xC0FFEE05u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

#define MAIL_CMD (g_mailbox[1])
#define MAIL_PAGE (g_mailbox[2])
#define MAIL_LED (g_mailbox[3])
#define MAIL_KEYCNT (g_mailbox[4])
#define MAIL_KEYNOW (g_mailbox[5])
#define MAIL_DIR (g_mailbox[6])
#define MAIL_SPEED (g_mailbox[7])
#define MAIL_PA15_HZ (g_mailbox[8])
#define MAIL_PB4_HZ (g_mailbox[9])
#define MAIL_PWM_PRESET (g_mailbox[10])
#define MAIL_PA7_HZ (g_mailbox[11])
#define MAIL_PA7_DUTY (g_mailbox[12])
#define MAIL_MV_R37 (g_mailbox[13])
#define MAIL_MV_R38 (g_mailbox[14])
#define MAIL_LOOP (g_mailbox[15])
#define MAIL_REDRAWS (g_mailbox[16])

// ---- registers (STM32G431, RM0440 / CMSIS) ----
#define RCC_BASE 0x40021000u
#define RCC_CR (*(volatile uint32_t*)(RCC_BASE + 0x00u))
#define RCC_CFGR (*(volatile uint32_t*)(RCC_BASE + 0x08u))
#define RCC_PLLCFGR (*(volatile uint32_t*)(RCC_BASE + 0x0Cu))
#define RCC_AHB2ENR (*(volatile uint32_t*)(RCC_BASE + 0x4Cu))
#define RCC_APB1ENR (*(volatile uint32_t*)(RCC_BASE + 0x58u))
#define RCC_APB2ENR (*(volatile uint32_t*)(RCC_BASE + 0x60u))
#define FLASH_ACR (*(volatile uint32_t*)0x40022000u)

#define TIM2 ((volatile uint32_t*)0x40000000u)
#define TIM3 ((volatile uint32_t*)0x40000400u)
#define TIM16 ((volatile uint32_t*)0x40014400u)
#define TIM_CR1(t) (t[0x00u / 4])
#define TIM_DIER(t) (t[0x0Cu / 4])
#define TIM_SR(t) (t[0x10u / 4])
#define TIM_EGR(t) (t[0x14u / 4])
#define TIM_CCMR1(t) (t[0x18u / 4])
#define TIM_CCER(t) (t[0x20u / 4])
#define TIM_PSC(t) (t[0x28u / 4])
#define TIM_ARR(t) (t[0x2Cu / 4])
#define TIM_CCR1(t) (t[0x34u / 4])
#define TIM_CCR2(t) (t[0x38u / 4])

#define ADC1 ((volatile uint32_t*)0x50000000u)
#define ADC2 ((volatile uint32_t*)0x50000100u)
#define ADC_ISR(a) (a[0x00u / 4])
#define ADC_CR(a) (a[0x08u / 4])
#define ADC_CFGR(a) (a[0x0Cu / 4])
#define ADC_SMPR1(a) (a[0x14u / 4])
#define ADC_SQR1(a) (a[0x30u / 4])
#define ADC_DR(a) (a[0x40u / 4])

#define NVIC_ISER0 (*(volatile uint32_t*)0xE000E100u)
#define SCB_CPACR (*(volatile uint32_t*)0xE000ED88u)
#define SYST_CSR (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018u)

#define ADC1_IN11_R38 11u  // PB12, R38 potentiometer
#define ADC2_IN15_R37 15u  // PB15, R37 potentiometer

#define PAGE_COUNT 3u

// ---- LED speed table (ms per chaser step) ----
static const uint32_t kSpeeds[4] = {400u, 200u, 100u, 50u};
// ---- PWM presets: ARR is computed from the 1 MHz counter, duty in permille ----
static const uint32_t kPwmHz[4] = {1000u, 2000u, 5000u, 10000u};
static const uint32_t kPwmDuty[4] = {500u, 250u, 750u, 100u};

// ---- state ----
static uint32_t s_page = 0;
static uint32_t s_pageDrawn = 0;
static uint32_t s_ledPattern = 0x01u;
static uint32_t s_ledBit = 0;
static uint32_t s_ledDir = 1;
static uint32_t s_speedIdx = 1;      // 200 ms
static uint32_t s_pwmPreset = 0;
static uint32_t s_patIdx = 0;

static volatile uint32_t keyCount[4];
// Debounced in the SysTick ISR (1 ms tick, 8 ms stability): s_keyPend holds the
// presses the main loop has not consumed yet, s_keyRaw/s_keyStable are the
// candidate/confirmed raw masks, s_keyRawMs is when the candidate last changed.
static volatile uint32_t s_keyPend[4];
static uint32_t s_keyRaw = 0, s_keyStable = 0, s_keyRawMs = 0;

// PA15 capture
static volatile uint32_t g_pa15_hz = 0, g_ovf = 0, g_cap_delta = 0;
static volatile uint32_t s_prevCap = 0, s_prevOvf = 0, s_havePrev = 0;
// PB4 capture
static volatile uint32_t g_pb4_hz = 0, g_pb4_ovf = 0, g_pb4_delta = 0;
static volatile uint32_t s_prevCap4 = 0, s_prevOvf4 = 0, s_havePrev4 = 0;

// ---------------------------------------------------------------------------
// 74LS573 LED bank (PC8..PC15 active low, latch enable PD2)
// ---------------------------------------------------------------------------
static void led_disp(uint32_t pattern) {
    uint32_t odr = GPIOC->ODR & 0x00FFu;  // keep the LCD data low byte
    odr |= (~pattern & 0xFFu) << 8;
    GPIOC->ODR = odr;
    GPIOD->BSRR = (1u << 2);
    GPIOD->BSRR = (1u << 2) << 16;        // latch
    s_ledPattern = pattern;
    MAIL_LED = pattern;
}

// ---------------------------------------------------------------------------
// interrupts
// ---------------------------------------------------------------------------
static uint32_t key_raw(void);  // defined below; sampled from the 1 ms tick

void SysTick_Handler(void) {
    g_uwTick++;
    // Key sampling lives HERE, not in the main loop: a full LCD redraw (page
    // clear + draw) keeps the main loop busy for tens of milliseconds of
    // virtual time, and every click landing in that window used to vanish.
    // The ISR only debounces and counts; the actions run in the main loop.
    const uint32_t raw = key_raw();
    if (raw != s_keyRaw) {
        s_keyRaw = raw;
        s_keyRawMs = g_uwTick;
    } else if ((g_uwTick - s_keyRawMs) >= 8u && raw != s_keyStable) {
        const uint32_t pressed = raw & ~s_keyStable;
        s_keyStable = raw;
        for (uint32_t i = 0; i < 4u; i++) {
            if (pressed & (1u << i)) s_keyPend[i]++;
        }
    }
    MAIL_KEYNOW = raw;
}

void TIM3_IRQHandler(void) { TIM_SR(TIM3) = ~1u; }  // PWM update: clear UIF

void TIM2_IRQHandler(void) {  // PA15 capture (CC1) + counter wrap (UIF)
    const uint32_t sr = TIM_SR(TIM2);
    uint32_t handled = 0;
    if (sr & 1u) {
        g_ovf++;
        handled |= 1u;
    }
    if (sr & 2u) {
        const uint32_t cap = TIM_CCR1(TIM2);
        if (s_havePrev) {
            const uint32_t ticks =
                (g_ovf - s_prevOvf) * 65536u + (cap - s_prevCap);
            g_cap_delta = ticks;
            if (ticks > 0u) g_pa15_hz = 1000000u / ticks;  // 1 MHz counter
        }
        s_prevCap = cap;
        s_prevOvf = g_ovf;
        s_havePrev = 1;
        handled |= 2u;
    }
    if (handled) TIM_SR(TIM2) = ~handled;
}

void TIM16_IRQHandler(void) {  // PB4 capture on TIM16_CH1
    const uint32_t sr = TIM_SR(TIM16);
    uint32_t handled = 0;
    if (sr & 1u) {
        g_pb4_ovf++;
        handled |= 1u;
    }
    if (sr & 2u) {
        const uint32_t cap = TIM_CCR1(TIM16);
        if (s_havePrev4) {
            const uint32_t ticks =
                (g_pb4_ovf - s_prevOvf4) * 65536u + (cap - s_prevCap4);
            g_pb4_delta = ticks;
            if (ticks > 0u) g_pb4_hz = 1000000u / ticks;
        }
        s_prevCap4 = cap;
        s_prevOvf4 = g_pb4_ovf;
        s_havePrev4 = 1;
        handled |= 2u;
    }
    if (handled) TIM_SR(TIM16) = ~handled;
}

void ADC1_2_IRQHandler(void) {}
void TIM6_IRQHandler(void) {}
void TIM7_IRQHandler(void) {}

// ---------------------------------------------------------------------------
// clock / time base
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

static void systick_init(void) {
    SYST_RVR = 79999u;                 // 1 ms
    SYST_CVR = 0;
    SYST_CSR = 7u;                     // ENABLE | TICKINT | CLKSOURCE
}

// ---------------------------------------------------------------------------
// ADC (HAL-equivalent register sequence)
// ---------------------------------------------------------------------------
static void adc_enable(volatile uint32_t* adc) {
    ADC_CR(adc) = 1u << 0;                       // ADEN
    while ((ADC_ISR(adc) & (1u << 0)) == 0) {}   // ADRDY
}
static void adc_calibrate(volatile uint32_t* adc) {
    ADC_CR(adc) = (1u << 31) | (1u << 28);       // ADCAL | ADVREGEN
    while ((ADC_CR(adc) & (1u << 31)) != 0) {}
    adc_enable(adc);
}
static void adc_setup_channel(volatile uint32_t* adc, uint32_t channel) {
    ADC_CFGR(adc) = 0;               // 12 bit, right aligned, single
    ADC_SMPR1(adc) = 0x7u;           // longest sampling
    ADC_SQR1(adc) = (channel << 6);  // rank 1
}
static uint32_t adc_convert(volatile uint32_t* adc) {
    ADC_CR(adc) = (1u << 0) | (1u << 2);          // ADEN | ADSTART
    uint32_t guard = 0;
    while ((ADC_ISR(adc) & (1u << 2)) == 0) {     // EOC
        if (++guard > 100000u) break;
    }
    return ADC_DR(adc);                           // reading DR clears EOC
}

// ---------------------------------------------------------------------------
// PWM output (PA7 = TIM3_CH2, AF2) and the two capture inputs
// ---------------------------------------------------------------------------
static void pwm_init(void) {
    RCC_APB1ENR |= (1u << 1);   // TIM3EN
    GPIOA->MODER = (GPIOA->MODER & ~(3u << (7 * 2))) | (2u << (7 * 2));
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(0xFu << (7 * 4))) | (2u << (7 * 4));

    TIM_CR1(TIM3) = 0;
    TIM_PSC(TIM3) = 79u;        // 1 MHz counter -> 1 us tick
    TIM_ARR(TIM3) = 999u;
    TIM_CCR2(TIM3) = 500u;
    TIM_CCMR1(TIM3) = ((0x6u << 4) | 0x8u) << 8;  // PWM1 + OC2PE
    TIM_CCER(TIM3) = 0x0010u;                     // CC2E
    TIM_EGR(TIM3) = 1u;
    TIM_SR(TIM3) = 0;
    TIM_DIER(TIM3) = 1u;                          // UIE
    NVIC_ISER0 = 1u << 29;                        // TIM3_IRQn
    TIM_CR1(TIM3) = 1u;                           // CEN
}

static void pwm_apply(uint32_t preset) {
    uint32_t arr = 1000000u / kPwmHz[preset];
    if (arr < 2u) arr = 2u;
    arr -= 1u;
    uint32_t ccr = ((arr + 1u) * kPwmDuty[preset] + 500u) / 1000u;
    if (ccr > arr) ccr = arr;
    TIM_ARR(TIM3) = arr;
    TIM_CCR2(TIM3) = ccr;
}

static void capture_init(void) {
    RCC_APB1ENR |= (1u << 0);   // TIM2EN
    GPIOA->MODER = (GPIOA->MODER & ~(3u << (15 * 2))) | (2u << (15 * 2));
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFu << ((15 - 8) * 4))) |
                    (1u << ((15 - 8) * 4));  // AF1 = TIM2_CH1
    TIM_CR1(TIM2) = 0;
    TIM_PSC(TIM2) = 79u;
    TIM_ARR(TIM2) = 0xFFFFu;
    TIM_CCMR1(TIM2) = 0x0001u;  // CC1S = input on TI1, rising
    TIM_CCER(TIM2) = 0x0001u;   // CC1E
    TIM_DIER(TIM2) = 0x0003u;   // UIE + CC1IE
    TIM_EGR(TIM2) = 1u;
    TIM_SR(TIM2) = 0;
    s_prevCap = 0;
    s_prevOvf = 0;
    g_ovf = 0;
    s_havePrev = 0;
    NVIC_ISER0 = 1u << 28;      // TIM2_IRQn
    TIM_CR1(TIM2) = 1u;
}

static void pb4_capture_init(void) {
    RCC_APB2ENR |= (1u << 16);  // TIM16EN
    GPIOB->MODER = (GPIOB->MODER & ~(3u << (4 * 2))) | (2u << (4 * 2));
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFu << (4 * 4))) | (1u << (4 * 4));
    TIM_CR1(TIM16) = 0;
    TIM_PSC(TIM16) = 79u;
    TIM_ARR(TIM16) = 0xFFFFu;
    TIM_CCMR1(TIM16) = 0x0001u;
    TIM_CCER(TIM16) = 0x0001u;
    TIM_DIER(TIM16) = 0x0003u;
    TIM_EGR(TIM16) = 1u;
    TIM_SR(TIM16) = 0;
    s_prevCap4 = 0;
    s_prevOvf4 = 0;
    g_pb4_ovf = 0;
    s_havePrev4 = 0;
    NVIC_ISER0 = 1u << 25;      // TIM16_IRQn
    TIM_CR1(TIM16) = 1u;
}

static void analog_pins_init(void) {
    // R38 -> PB12 (ADC1_IN11), R37 -> PB15 (ADC2_IN15): analog mode
    GPIOB->MODER |= (3u << (12 * 2)) | (3u << (15 * 2));
}

// ---------------------------------------------------------------------------
// keys: B1=PB0, B2=PB1, B3=PB2, B4=PA0 (active low, external pull-ups).
// Scanned every 2 ms with 8 ms of stability required: a normal debounce (real
// switches bounce for <5 ms) that still catches a short click. The first
// version used 5 ms / 20 ms, which silently dropped fast presses -- the GUI
// click lasts ~30 ms of wall time, and the simulator runs at <1x real time, so
// a 20 ms virtual threshold ate half of them.
// ---------------------------------------------------------------------------
static uint32_t key_raw(void) {
    const uint32_t idrB = GPIOB->IDR;
    const uint32_t idrA = GPIOA->IDR;
    uint32_t m = 0;
    if (!(idrB & (1u << 0))) m |= 1u;
    if (!(idrB & (1u << 1))) m |= 2u;
    if (!(idrB & (1u << 2))) m |= 4u;
    if (!(idrA & (1u << 0))) m |= 8u;
    return m;
}

static void key_action(uint32_t index) {
    switch (index) {
    case 0:  // B1: LED direction
        s_ledDir ^= 1u;
        break;
    case 1:  // B2: LED speed
        s_speedIdx = (s_speedIdx + 1u) & 3u;
        break;
    case 2:  // B3: PWM preset
        s_pwmPreset = (s_pwmPreset + 1u) & 3u;
        pwm_apply(s_pwmPreset);
        break;
    default:  // B4: next page
        s_page = (s_page + 1u) % PAGE_COUNT;
        s_pageDrawn = 0;
        break;
    }
    MAIL_DIR = s_ledDir;
    MAIL_SPEED = kSpeeds[s_speedIdx];
    MAIL_PWM_PRESET = s_pwmPreset;
}

// Consume the presses the 1 ms tick detected: count them and run the actions
// (actions are deferred here on purpose -- they can trigger an LCD redraw,
// which must not run inside an interrupt).
static void key_consume(void) {
    for (uint32_t i = 0; i < 4u; i++) {
        while (s_keyPend[i] != 0u) {
            s_keyPend[i] = s_keyPend[i] - 1u;
            keyCount[i]++;
            key_action(i);
        }
    }
    MAIL_KEYCNT = keyCount[0] | (keyCount[1] << 8) | (keyCount[2] << 16) |
                  (keyCount[3] << 24);
}

// ---------------------------------------------------------------------------
// tiny formatting (no libc in this firmware)
// ---------------------------------------------------------------------------
static int put_uint(char* dst, uint32_t v, int minDigits) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n < minDigits) tmp[n++] = '0';
    int out = 0;
    while (n > 0) dst[out++] = tmp[--n];
    return out;
}

static void append(char* buf, int* n, const char* s) {
    while (*s) buf[(*n)++] = *s++;
}

// ---------------------------------------------------------------------------
// LCD pages
// ---------------------------------------------------------------------------
static void lcd_setup_colors(void) {
    LCD_SetBackColor(Black);
    LCD_SetTextColor(Green);
}

// ---------------------------------------------------------------------------
// Panel-space drawing helpers.
//
// The official BSP draws in the controller's NATIVE frame: LCD_SetCursor writes
// R32 = Xpos (which is the panel's VERTICAL axis) and R33 = Ypos (the panel's
// HORIZONTAL axis, mirrored: panel x = 319 - Ypos). The string helpers already
// compensate for that internally (LCD_DisplayStringLine walks refcolumn 319 ->
// 0), which is why text comes out upright, but the geometry primitives do not.
// These wrappers apply the same mapping so the page code below can think in
// plain panel pixels (px, py = top-left corner, w/h in panel pixels).
// ---------------------------------------------------------------------------
static void rect_panel(uint16_t px, uint16_t py, uint16_t w, uint16_t h) {
    LCD_DrawRect((u8)py, (u16)(319u - px), (u8)h, w);
}

static void circle_panel(uint16_t px, uint16_t py, uint16_t r) {
    LCD_DrawCircle((u8)py, (u16)(319u - px), r);
}

// Filled panel-space rectangle. Built from panel-horizontal runs: the BSP's
// DrawLine with Direction == Horizontal advances the GRAM address counter along
// the panel's horizontal axis (that is the same increment the string helper
// relies on), one Prepare + Length writes per row. The window-mode path
// (R50..R53) writes in controller order and does NOT land in panel space, so it
// is deliberately not used here.
static void fill_panel(uint16_t px, uint16_t py, uint16_t w, uint16_t h,
                       uint16_t colour) {
    LCD_SetTextColor(colour);  // DrawLine paints with TextColor
    for (uint16_t r = 0; r < h; r++) {
        LCD_DrawLine((u8)(py + r), (u16)(319u - px), w, Horizontal);
    }
}

// ---------------------------------------------------------------------------
// Line cache: only rewrite lines whose TEXT actually changed.
//
// The official BSP renders every character as 16x24 pixels straight through the
// GPIO bus (~3 bus writes per pixel), so one 24-px text line costs ~6-8 ms of
// real time in the simulator. Refreshing all seven status lines every 500 ms
// meant the screen was busy drawing ~10% of the time and looked sluggish (the
// user notices because the simulation is 1:1 with real time). With the cache a
// normal refresh touches only the one or two lines that really changed.
// ---------------------------------------------------------------------------
static const uint8_t kLines[10] = {Line0, Line1, Line2, Line3, Line4,
                                   Line5, Line6, Line7, Line8, Line9};
static char s_lineCache[10][24];
static uint16_t s_lineValid = 0;  // bit i = s_lineCache[i] matches the screen

static void lcd_invalidate(void) { s_lineValid = 0; }

static void lcd_line(uint8_t line, const char* text) {
    char* old = s_lineCache[line];
    if (s_lineValid & (1u << line)) {
        const char* p = text;
        const char* q = old;
        int same = 1;
        while (*p != 0 && *q != 0) {
            if (*p++ != *q++) {
                same = 0;
                break;
            }
        }
        if (same && *p == 0 && *q == 0) return;  // already on screen
    }
    LCD_DisplayStringLine(kLines[line], (u8*)text);
    int i = 0;
    while (text[i] != 0 && i < 23) {
        old[i] = text[i];
        i++;
    }
    old[i] = 0;
    s_lineValid |= (uint16_t)(1u << line);
}

static void lcd_page0_values(uint32_t mv37, uint32_t mv38, uint32_t nowMask) {
    char buf[24];
    int n;

    n = 0;
    append(buf, &n, "LED p");
    n += put_uint(buf + n, s_ledBit + 1u, 1);
    append(buf, &n, s_ledDir ? " > " : " < ");
    n += put_uint(buf + n, kSpeeds[s_speedIdx], 1);
    append(buf, &n, "ms");
    buf[n] = 0;
    lcd_line(2, buf);

    n = 0;
    append(buf, &n, "KEY CNT ");
    for (uint32_t i = 0; i < 4u; i++) {
        n += put_uint(buf + n, keyCount[i], 1);
        buf[n++] = ' ';
    }
    buf[n] = 0;
    lcd_line(3, buf);

    n = 0;
    append(buf, &n, "KEY NOW ");
    for (uint32_t i = 0; i < 4u; i++) buf[n++] = (nowMask & (1u << i)) ? '1' : '-';
    buf[n] = 0;
    lcd_line(4, buf);

    n = 0;
    append(buf, &n, "IN PA15 ");
    n += put_uint(buf + n, g_pa15_hz, 1);
    append(buf, &n, "Hz");
    buf[n] = 0;
    lcd_line(5, buf);

    n = 0;
    append(buf, &n, "IN PB4  ");
    n += put_uint(buf + n, g_pb4_hz, 1);
    append(buf, &n, "Hz");
    buf[n] = 0;
    lcd_line(6, buf);

    n = 0;
    append(buf, &n, "OUT PA7 ");
    n += put_uint(buf + n, 1000000u / (TIM_ARR(TIM3) + 1u), 1);
    append(buf, &n, "Hz P");
    n += put_uint(buf + n, s_pwmPreset + 1u, 1);
    buf[n] = 0;
    lcd_line(7, buf);

    n = 0;
    append(buf, &n, "R37 ");
    n += put_uint(buf + n, mv37 / 1000u, 1);
    buf[n++] = '.';
    n += put_uint(buf + n, (mv37 % 1000u) / 10u, 2);
    append(buf, &n, "V R38 ");
    n += put_uint(buf + n, mv38 / 1000u, 1);
    buf[n++] = '.';
    n += put_uint(buf + n, (mv38 % 1000u) / 10u, 2);
    append(buf, &n, "V");
    buf[n] = 0;
    lcd_line(8, buf);
}

static void lcd_page_header(const char* title) {
    lcd_setup_colors();
    LCD_SetTextColor(Green);
    lcd_line(0, title);
    LCD_SetTextColor(White);
    lcd_line(1, "B4: next page");
    MAIL_REDRAWS++;
}

static void lcd_page1_draw(void) {  // LCD geometry / colour test
    LCD_Clear(Black);
    lcd_page_header("LCD TEST  P1/2");
    // 8 filled colour bars across the panel (40 x 40 px each)
    static const uint16_t bar[8] = {White, Red, Green, Blue,
                                    Yellow, Magenta, Cyan, Grey};
    for (uint32_t i = 0; i < 8u; i++) {
        fill_panel((uint16_t)(i * 40u), 52u, 36u, 36u, bar[i]);
    }
    // bordered rectangle + circle, both in panel coordinates
    LCD_SetTextColor(White);
    rect_panel(4u, 100u, 120u, 40u);
    LCD_SetTextColor(Cyan);
    circle_panel(240u, 190u, 34u);
    LCD_SetTextColor(Red);
    lcd_line(6, "RED");
    LCD_SetTextColor(Yellow);
    lcd_line(7, "YELLOW");
    LCD_SetTextColor(White);
}

static void lcd_page2_draw(const char* patternName) {  // LED pattern test
    LCD_Clear(Black);
    lcd_page_header("LED TEST  P2/2");
    LCD_SetTextColor(Yellow);
    lcd_line(3, "PATTERN");
    LCD_SetTextColor(White);
    lcd_line(5, patternName);
    LCD_SetTextColor(Cyan);
    lcd_line(8, "B2: speed  B1: dir");
    LCD_SetTextColor(White);
}

// Periodic refresh for page 2: the line cache makes this a no-op unless the
// pattern name really changed (a full-screen redraw here would cost ~30 ms).
static void lcd_page2_refresh(const char* patternName) {
    lcd_line(5, patternName);
}

static const char* pat_name(uint32_t idx) {
    switch (idx) {
    case 1: return "ALL ON";
    case 2: return "ALT 55/AA";
    default: return "WALK";
    }
}

int main(void) {
    SCB_CPACR = (0xFu << 20);
    __asm volatile("dsb");
    __asm volatile("isb");

    clock_init();
    systick_init();

    // GPIO clocks: every port used here needs its clock (real hardware and the
    // model both drop pin writes without it)
    RCC_AHB2ENR |= (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
    (void)RCC_AHB2ENR;

    // LEDs (74LS573 latch on PD2)
    GPIOD->MODER = (GPIOD->MODER & ~(3u << (2 * 2))) | (1u << (2 * 2));
    GPIOD->BSRR = (1u << 2) << 16;
    led_disp(0);

    // keys: plain inputs (the board has external pull-ups)
    GPIOA->MODER &= ~(3u << (0 * 2));    // PA0 = B4
    GPIOB->MODER &= ~((3u << (0 * 2)) | (3u << (1 * 2)) | (3u << (2 * 2)));

    LCD_Init();
    analog_pins_init();
    adc_enable(ADC1);
    adc_calibrate(ADC1);
    adc_setup_channel(ADC1, ADC1_IN11_R38);
    adc_enable(ADC2);
    adc_calibrate(ADC2);
    adc_setup_channel(ADC2, ADC2_IN15_R37);

    pwm_init();
    capture_init();
    pb4_capture_init();

    MAIL_PAGE = 0;
    MAIL_DIR = s_ledDir;
    MAIL_SPEED = kSpeeds[s_speedIdx];
    MAIL_PWM_PRESET = 0;

    uint32_t lastTickMs = 0, lastLcdMs = 0, lastChaseMs = 0, lastPatMs = 0;
    uint32_t mv37 = 0, mv38 = 0;
    uint32_t lastCmd = 0;

    for (;;) {
        MAIL_LOOP++;

        // ---- host command (headless tests switch pages this way) ----
        const uint32_t cmd = MAIL_CMD;
        if (cmd != lastCmd) {
            lastCmd = cmd;
            s_page = cmd % PAGE_COUNT;
            s_pageDrawn = 0;
        }

        // ---- keys: debounced in the 1 ms tick, actions consumed here ----
        key_consume();

        // ---- 100 Hz control tick (low MMIO density keeps the sim fast) ----
        if ((g_uwTick - lastTickMs) >= 10u) {
            lastTickMs = g_uwTick;

            // ADC: both potentiometers
            const uint32_t codeR37 = adc_convert(ADC2);
            const uint32_t codeR38 = adc_convert(ADC1);
            mv37 = (codeR37 * 3300u) / 4095u;
            mv38 = (codeR38 * 3300u) / 4095u;
            MAIL_MV_R37 = mv37;
            MAIL_MV_R38 = mv38;
            MAIL_PA15_HZ = g_pa15_hz;
            MAIL_PB4_HZ = g_pb4_hz;
            MAIL_PA7_HZ = 1000000u / (TIM_ARR(TIM3) + 1u);
            MAIL_PA7_DUTY = (TIM_CCR2(TIM3) * 1000u) / (TIM_ARR(TIM3) + 1u);
            MAIL_PAGE = s_page;

            // ---- LED driving ----
            if (s_page == 2u) {
                // page 2: cycle the three LED patterns every 400 ms
                if ((g_uwTick - lastPatMs) >= 400u) {
                    lastPatMs = g_uwTick;
                    s_patIdx = (s_patIdx + 1u) % 3u;
                    if (s_patIdx == 1u) led_disp(0xFFu);  // ALL ON
                }
                if (s_patIdx == 0u) {  // walk pattern still runs on page 2
                    if ((g_uwTick - lastChaseMs) >= kSpeeds[s_speedIdx]) {
                        lastChaseMs = g_uwTick;
                        s_ledBit = (s_ledBit + 8u + (s_ledDir ? 1u : 7u)) % 8u;
                        led_disp(1u << s_ledBit);
                    }
                } else if (s_patIdx == 2u) {
                    static uint32_t altPhase = 0;
                    if ((g_uwTick - lastChaseMs) >= 200u) {
                        lastChaseMs = g_uwTick;
                        altPhase ^= 1u;
                        led_disp(altPhase ? 0xAAu : 0x55u);
                    }
                }
            } else {
                // pages 0/1: LED chaser (B1 = direction, B2 = speed)
                if ((g_uwTick - lastChaseMs) >= kSpeeds[s_speedIdx]) {
                    lastChaseMs = g_uwTick;
                    s_ledBit = (s_ledBit + 8u + (s_ledDir ? 1u : 7u)) % 8u;
                    led_disp(1u << s_ledBit);
                }
            }
        }

        // ---- LCD: full redraw on page switch, values every 500 ms ----
        // NOTE: deliberately NOT wrapped in cpsid/cpsie -- the 1 ms tick must
        // keep sampling the keys while the LCD is drawn (an LCD redraw is tens
        // of milliseconds of virtual time). Only this loop touches GPIOC/LCD.
        if (!s_pageDrawn) {
            s_pageDrawn = 1;
            LCD_Clear(Black);
            lcd_invalidate();  // the screen is blank: every cached line is stale
            if (s_page == 0u) {
                lcd_page_header("SELF TEST  P0/2");
            } else if (s_page == 1u) {
                lcd_page1_draw();
            } else {
                lcd_page2_draw(pat_name(s_patIdx));
            }
        } else if ((g_uwTick - lastLcdMs) >= 500u) {
            lastLcdMs = g_uwTick;
            if (s_page == 0u) {
                lcd_page0_values(mv37, mv38, s_keyStable);
            } else if (s_page == 2u) {
                lcd_page2_refresh(pat_name(s_patIdx));
            }
            // page 1 is static art: drawn once, nothing to refresh
        }
    }
}