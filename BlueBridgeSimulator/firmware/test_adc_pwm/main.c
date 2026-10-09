// CT117E-M4 ADC / PWM / capture closed-loop firmware (bare metal + official
// LCD BSP) -- a blue-bridge style "measure and control" program.
//
// What it does, entirely through real memory-mapped registers:
//   R37 (ADC2_IN15 / PB15) -> duty  of the PA7 PWM      (duty = R37/3.3 * 100 %)
//   R38 (ADC1_IN11 / PB12) -> freq  of the PA7 PWM      (1 kHz + R38/3.3 * 4 kHz)
//   PA15 pulse input       -> TIM2_CH1 input capture    -> firmware measures Hz
//   PB4  pulse input       -> TIM16_CH1 input capture   -> firmware measures Hz
//   firmware compares PA15 vs PA7 frequency: |diff| > 1000 Hz -> ALARM
//   (ALARM lights all LEDs and shows ALARM on the LCD -- the firmware decides,
//    the simulator never does)
//
// The LCD shows the potentiometer voltages, the measured input frequency and
// the PWM the firmware configured, so the GUI knobs (R37/R38/PA15) have an
// observable effect on a real firmware.
//
// Test phases (mailbox command word, used by the headless self test):
//   0  closed loop        (default: ADC -> duty/frequency, capture -> compare)
//   1  wrong AF           (PWM configured, PA7 left as a plain input)
//   2  fixed PWM         1000 Hz / 50 %
//   3  fixed PWM         2000 Hz / 25 %
//
// Mailbox at 0x20000000 (.selfdata):
//   [0]  magic 0xC0FFEE04      [1] cmd            [2] phase
//   [3]  ADC2 (R37) raw code   [4] ADC1 (R38) raw code
//   [5]  R37 millivolts        [6] R38 millivolts
//   [7]  PA15 measured Hz      [8] PA7 configured Hz
//   [9]  PA7 duty permille     [10] alarm (0/1)   [11] capture delta (counts)
//   [12] TIM2 counter overflows [13] main loop count
#include "lcd.h"  // official CT117E LCD BSP (brings in main.h)

__attribute__((section(".selfdata"), used))
volatile uint32_t g_mailbox[17] = {
    0xC0FFEE04u,  // magic
    0,            // cmd (host -> firmware)
    0,            // phase (firmware -> host)
    0,            // ADC2 = R37 raw
    0,            // ADC1 = R38 raw
    0,            // R37 mV
    0,            // R38 mV
    0,            // PA15 Hz (measured)
    0,            // PA7 Hz (configured)
    0,            // PA7 duty permille
    0,            // alarm
    0,            // PA15 capture delta (counter ticks)
    0,            // TIM2 overflows
    0,            // main loop count
    0,            // PB4 Hz (measured)
    0,            // PB4 capture delta
    0,            // TIM16 overflows
};

#define MAIL_CMD (g_mailbox[1])
#define MAIL_PHASE (g_mailbox[2])
#define MAIL_ADC_R37 (g_mailbox[3])
#define MAIL_ADC_R38 (g_mailbox[4])
#define MAIL_MV_R37 (g_mailbox[5])
#define MAIL_MV_R38 (g_mailbox[6])
#define MAIL_PA15_HZ (g_mailbox[7])
#define MAIL_PA7_HZ (g_mailbox[8])
#define MAIL_PA7_DUTY (g_mailbox[9])
#define MAIL_ALARM (g_mailbox[10])
#define MAIL_DELTA (g_mailbox[11])
#define MAIL_OVF (g_mailbox[12])
#define MAIL_LOOP (g_mailbox[13])
#define MAIL_PB4_HZ (g_mailbox[14])
#define MAIL_PB4_DELTA (g_mailbox[15])
#define MAIL_PB4_OVF (g_mailbox[16])

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

// ADC1 = 0x50000000, ADC2 = 0x50000100 (CMSIS ADC_TypeDef layout)
#define ADC1 ((volatile uint32_t*)0x50000000u)
#define ADC2 ((volatile uint32_t*)0x50000100u)
#define ADC_ISR(a) (a[0x00u / 4])
#define ADC_IER(a) (a[0x04u / 4])
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

static volatile uint32_t g_pa15_hz = 0;      // measured input frequency (PA15)
static volatile uint32_t g_cap_delta = 0;    // last capture period in ticks
static volatile uint32_t g_ovf = 0;          // TIM2 counter wrap count
static volatile uint32_t s_prevCap = 0;
static volatile uint32_t s_prevOvf = 0;
static volatile uint32_t s_havePrev = 0;

static volatile uint32_t g_pb4_hz = 0;       // measured input frequency (PB4)
static volatile uint32_t g_pb4_delta = 0;
static volatile uint32_t g_pb4_ovf = 0;      // TIM16 counter wrap count
static volatile uint32_t s_prevCap4 = 0;
static volatile uint32_t s_prevOvf4 = 0;
static volatile uint32_t s_havePrev4 = 0;

// ---------------------------------------------------------------------------
// 74LS573 LED bank (PC8..PC15 active low, latch enable PD2)
// ---------------------------------------------------------------------------
static void led_disp(uint32_t pattern) {
    uint32_t odr = GPIOC->ODR & 0x00FFu;  // keep the LCD data low byte
    odr |= (~pattern & 0xFFu) << 8;
    GPIOC->ODR = odr;
    GPIOD->BSRR = (1u << 2);
    GPIOD->BSRR = (1u << 2) << 16;
}

// ---------------------------------------------------------------------------
// interrupts
// ---------------------------------------------------------------------------
void SysTick_Handler(void) { g_uwTick++; }

void TIM3_IRQHandler(void) {
    TIM_SR(TIM3) = ~1u;  // update event: clear UIF (write 0 to clear)
}

// PA15 capture (CC1) + counter overflow (UIF) on the same IRQ line
void TIM2_IRQHandler(void) {
    const uint32_t sr = TIM_SR(TIM2);
    uint32_t handled = 0;
    if (sr & 1u) {  // counter wrapped: the firmware counts it itself
        g_ovf++;
        handled |= 1u;
    }
    if (sr & 2u) {  // capture: CCR1 holds CNT at the edge
        const uint32_t cap = TIM_CCR1(TIM2);
        if (s_havePrev) {
            // period in 1 us ticks, corrected with the wrap count
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

// PB4 capture on TIM16_CH1 (CC1) + counter overflow, same IRQ line (TIM16 = 25)
void TIM16_IRQHandler(void) {
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

void ADC1_2_IRQHandler(void) { /* polling firmware uses no ADC interrupt */ }

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
    SYST_RVR = 79999u;
    SYST_CVR = 0;
    SYST_CSR = 7u;
}

// ---------------------------------------------------------------------------
// ADC init: exactly the HAL sequence (HAL_ADC_Init / Calibration_Start /
// ConfigChannel), written as registers so the model must implement the same
// semantics a HAL firmware relies on.
// ---------------------------------------------------------------------------
static void adc_enable(volatile uint32_t* adc) {
    // HAL_ADC_Init -> ADC_Enable(): ADEN, wait for ADRDY
    ADC_CR(adc) = 1u << 0;                       // ADEN
    while ((ADC_ISR(adc) & (1u << 0)) == 0) {}   // ADRDY
}
static void adc_calibrate(volatile uint32_t* adc) {
    // HAL_ADCEx_Calibration_Start(): ADCAL, wait until it self-clears
    ADC_CR(adc) = (1u << 31) | (1u << 28);       // ADCAL | ADVREGEN
    while ((ADC_CR(adc) & (1u << 31)) != 0) {}
    adc_enable(adc);                             // ADEN dropped during cal
}
static void adc_setup_channel(volatile uint32_t* adc, uint32_t channel) {
    ADC_CFGR(adc) = 0;              // 12 bit, right aligned, single conversion
    ADC_SMPR1(adc) = 0x7u;          // longest sampling time for all channels
    ADC_SQR1(adc) = (channel << 6); // rank 1 = the channel to convert
}
static uint32_t adc_convert(volatile uint32_t* adc) {
    // HAL_ADC_Start + HAL_ADC_PollForConversion + HAL_ADC_GetValue
    ADC_CR(adc) = (1u << 0) | (1u << 2);          // ADEN | ADSTART
    uint32_t guard = 0;
    while ((ADC_ISR(adc) & (1u << 2)) == 0) {     // EOC
        if (++guard > 100000u) break;
    }
    return ADC_DR(adc);                           // reading DR clears EOC
}

// ---------------------------------------------------------------------------
// PWM / capture
// ---------------------------------------------------------------------------
static void pa7_enable_af2(void);
static void pa7_disable_af(void);

static void pwm_init(uint32_t arr, uint32_t ccr) {
    RCC_APB1ENR |= (1u << 1);   // TIM3EN
    pa7_enable_af2();           // PA7 -> TIM3_CH2 (CubeMX would do this in GPIO init)
    TIM_CR1(TIM3) = 0;
    TIM_PSC(TIM3) = 79u;        // 1 MHz counter -> 1 us tick
    TIM_ARR(TIM3) = arr;
    TIM_CCR2(TIM3) = ccr;
    TIM_CCMR1(TIM3) = ((0x6u << 4) | 0x8u) << 8;  // PWM1 + OC2PE on channel 2
    TIM_CCER(TIM3) = 0x0010u;                     // CC2E
    TIM_EGR(TIM3) = 1u;                           // UG
    TIM_SR(TIM3) = 0;
    TIM_DIER(TIM3) = 1u;                          // UIE (PWM update events)
    NVIC_ISER0 = 1u << 29;                        // TIM3_IRQn
    TIM_CR1(TIM3) = 1u;                           // CEN
}

static void pwm_set(uint32_t arr, uint32_t ccr) {
    TIM_ARR(TIM3) = arr;    // takes effect immediately (ARPE = 0)
    TIM_CCR2(TIM3) = ccr;   // preloaded (OC2PE = 1): next period
}

static void pa7_enable_af2(void) {
    GPIOA->MODER = (GPIOA->MODER & ~(3u << (7 * 2))) | (2u << (7 * 2));
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(0xFu << (7 * 4))) | (2u << (7 * 4));
}
static void pa7_disable_af(void) {
    GPIOA->MODER &= ~(3u << (7 * 2));  // plain input: the PWM stays internal
}

static void pa15_enable_af1(void) {
    GPIOA->MODER = (GPIOA->MODER & ~(3u << (15 * 2))) | (2u << (15 * 2));
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFu << ((15 - 8) * 4))) |
                    (1u << ((15 - 8) * 4));  // AF1 = TIM2_CH1
}

static void pa1518_analog_12_15(void) {
    // PB12 (R38 -> ADC1_IN11) and PB15 (R37 -> ADC2_IN15): analog mode
    GPIOB->MODER |= (3u << (12 * 2)) | (3u << (15 * 2));
    GPIOB->PUPDR &= ~((3u << (12 * 2)) | (3u << (15 * 2)));
}

static void capture_init(void) {
    RCC_APB1ENR |= (1u << 0);   // TIM2EN
    pa15_enable_af1();
    TIM_CR1(TIM2) = 0;
    TIM_PSC(TIM2) = 79u;        // 1 MHz -> 1 us tick
    TIM_ARR(TIM2) = 0xFFFFu;
    TIM_CCMR1(TIM2) = 0x0001u;  // CC1S = 01 (input, TI1), rising edge
    TIM_CCER(TIM2) = 0x0001u;   // CC1E, CC1P = 0
    TIM_DIER(TIM2) = 0x0003u;   // UIE (overflow count) + CC1IE
    TIM_EGR(TIM2) = 1u;
    TIM_SR(TIM2) = 0;
    s_prevCap = 0;
    s_prevOvf = 0;
    g_ovf = 0;
    s_havePrev = 0;
    NVIC_ISER0 = 1u << 28;
    TIM_CR1(TIM2) = 1u;         // CEN
}

// PB4 -> TIM16_CH1 (AF1): second pulse input, 1 MHz counter, rising edge
static void pb4_capture_init(void) {
    RCC_APB2ENR |= (1u << 16);   // TIM16EN (APB2)
    // PB4 in alternate function mode, AF1 (AFRL bits 16..19)
    GPIOB->MODER = (GPIOB->MODER & ~(3u << (4 * 2))) | (2u << (4 * 2));
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFu << (4 * 4))) | (1u << (4 * 4));

    TIM_CR1(TIM16) = 0;
    TIM_PSC(TIM16) = 79u;        // 1 MHz -> 1 us tick
    TIM_ARR(TIM16) = 0xFFFFu;
    TIM_CCMR1(TIM16) = 0x0001u;  // CC1S = 01 (input, TI1)
    TIM_CCER(TIM16) = 0x0001u;   // CC1E, rising edge
    TIM_DIER(TIM16) = 0x0003u;   // UIE + CC1IE
    TIM_EGR(TIM16) = 1u;
    TIM_SR(TIM16) = 0;
    s_prevCap4 = 0;
    s_prevOvf4 = 0;
    g_pb4_ovf = 0;
    s_havePrev4 = 0;
    NVIC_ISER0 = 1u << 25;       // TIM16_IRQn
    TIM_CR1(TIM16) = 1u;         // CEN
}

// ---------------------------------------------------------------------------
// tiny formatting helpers (no libc in this firmware)
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

static void lcd_show_phase(uint32_t phase, uint32_t mv37, uint32_t mv38,
                           uint32_t inHz, uint32_t pb4Hz, uint32_t setHz,
                           uint32_t dutyPm, uint32_t alarm) {
    char buf[22];
    int n;

    n = 0;
    buf[n++] = 'R';
    buf[n++] = '3';
    buf[n++] = '7';
    buf[n++] = ':';
    buf[n++] = ' ';
    n += put_uint(buf + n, mv37 / 1000u, 1);
    buf[n++] = '.';
    n += put_uint(buf + n, (mv37 % 1000u) / 10u, 2);
    buf[n++] = 'V';
    buf[n++] = ' ';
    buf[n++] = 'D';
    buf[n++] = ':';
    n += put_uint(buf + n, dutyPm / 10u, 2);
    buf[n++] = '%';
    buf[n] = 0;
    LCD_DisplayStringLine(Line2, (u8*)buf);

    n = 0;
    buf[n++] = 'R';
    buf[n++] = '3';
    buf[n++] = '8';
    buf[n++] = ':';
    buf[n++] = ' ';
    n += put_uint(buf + n, mv38 / 1000u, 1);
    buf[n++] = '.';
    n += put_uint(buf + n, (mv38 % 1000u) / 10u, 2);
    buf[n++] = 'V';
    buf[n++] = ' ';
    buf[n++] = 'F';
    buf[n++] = ':';
    n += put_uint(buf + n, setHz, 1);
    buf[n++] = 'H';
    buf[n++] = 'z';
    buf[n] = 0;
    LCD_DisplayStringLine(Line4, (u8*)buf);

    n = 0;
    buf[n++] = 'P';
    buf[n++] = 'A';
    buf[n++] = '1';
    buf[n++] = '5';
    buf[n++] = ':';
    buf[n++] = ' ';
    n += put_uint(buf + n, inHz, 1);
    buf[n++] = ' ';
    buf[n++] = 'H';
    buf[n++] = 'z';
    buf[n] = 0;
    LCD_DisplayStringLine(Line6, (u8*)buf);

    n = 0;
    buf[n++] = 'P';
    buf[n++] = 'B';
    buf[n++] = '4';
    buf[n++] = ':';
    buf[n++] = ' ';
    n += put_uint(buf + n, pb4Hz, 1);
    buf[n++] = ' ';
    buf[n++] = 'H';
    buf[n++] = 'z';
    buf[n] = 0;
    LCD_DisplayStringLine(Line7, (u8*)buf);

    n = 0;
    buf[n++] = 'P';
    buf[n++] = 'A';
    buf[n++] = '7';
    buf[n++] = ':';
    buf[n++] = ' ';
    n += put_uint(buf + n, setHz, 1);
    buf[n++] = ' ';
    buf[n++] = 'H';
    buf[n++] = 'z';
    buf[n] = 0;
    if (phase == 1) {
        // wrong AF phase: the firmware's PWM stays internal, nothing reaches
        // the pin -- say so on the LCD instead of claiming an output
        LCD_DisplayStringLine(Line8, (u8*)"PA7: NO AF (internal only)");
    } else {
        LCD_DisplayStringLine(Line8, (u8*)buf);
    }

    LCD_DisplayStringLine(Line9, (u8*)(alarm ? "STATUS: ALARM" : "STATUS: OK  "));
}

// ---------------------------------------------------------------------------
// phase handling (host driven through the mailbox for the headless tests)
// ---------------------------------------------------------------------------
static uint32_t apply_phase(uint32_t phase) {
    switch (phase) {
    case 1:  // wrong AF: PWM configured, PA7 not routed
        pa7_disable_af();
        pwm_set(999u, 500u);
        break;
    case 2:  // fixed 1000 Hz / 50 %
        pa7_enable_af2();
        pwm_set(999u, 500u);
        break;
    case 3:  // fixed 2000 Hz / 25 %
        pa7_enable_af2();
        pwm_set(499u, 125u);
        break;
    default:  // closed loop: PWM follows the potentiometers
        pa7_enable_af2();
        break;
    }
    return phase;
}

int main(void) {
    SCB_CPACR = (0xFu << 20);
    __asm volatile("dsb");
    __asm volatile("isb");

    clock_init();
    systick_init();

    // GPIOD clock (74LS573 latch on PD2) - writes are dropped without it
    RCC_AHB2ENR |= (1u << 3);
    (void)RCC_AHB2ENR;
    (void)RCC_AHB2ENR;
    GPIOD->MODER = (GPIOD->MODER & ~(3u << (2 * 2))) | (1u << (2 * 2));
    GPIOD->BSRR = (1u << 2) << 16;
    led_disp(0);

    // LCD (official BSP) + static header
    LCD_Init();
    LCD_Clear(Black);
    LCD_SetBackColor(Black);
    LCD_SetTextColor(Green);
    LCD_DisplayStringLine(Line0, (u8*)"ADC/PWM CLOSED LOOP");
    LCD_SetTextColor(White);

    // analog pins + ADC setup (HAL sequence) + PWM + capture
    pa1518_analog_12_15();
    adc_enable(ADC1);
    adc_calibrate(ADC1);
    adc_setup_channel(ADC1, ADC1_IN11_R38);
    adc_enable(ADC2);
    adc_calibrate(ADC2);
    adc_setup_channel(ADC2, ADC2_IN15_R37);

    pwm_init(999u, 500u);
    capture_init();
    pb4_capture_init();

    uint32_t phase = 0;
    MAIL_PHASE = 0;
    uint32_t lastLedMs = 0, ledOn = 0, lastAdcMs = 0, lastLcdMs = 0;
    uint32_t prevAlarm = 0;
    uint32_t codeR37 = 0, codeR38 = 0, mvR37 = 0, mvR38 = 0;
    uint32_t realHz = 1000u, realDutyPm = 500u, alarm = 0;
    uint32_t pb4Hz = 0;

    for (;;) {
        MAIL_LOOP++;

        // ---- host command (test phases) ----
        const uint32_t cmd = MAIL_CMD;
        if (cmd != phase) {
            phase = apply_phase(cmd);
            MAIL_PHASE = phase;
        }

        // ---- control loop at 100 Hz: ADC -> PWM -> compare -> LED ----
        // (kept off the instruction-by-instruction path so the firmware stays
        //  fast in the simulator: one MMIO pass every 10 ms)
        if ((g_uwTick - lastAdcMs) >= 10u) {
            lastAdcMs = g_uwTick;

            // ADC: R37 (ADC2_IN15) and R38 (ADC1_IN11)
            codeR37 = adc_convert(ADC2);
            codeR38 = adc_convert(ADC1);
            mvR37 = (codeR37 * 3300u) / 4095u;
            mvR38 = (codeR38 * 3300u) / 4095u;
            MAIL_ADC_R37 = codeR37;
            MAIL_ADC_R38 = codeR38;
            MAIL_MV_R37 = mvR37;
            MAIL_MV_R38 = mvR38;

            // PWM target from the potentiometers (closed loop)
            uint32_t setHz, dutyPm;
            if (phase == 2) {
                setHz = 1000u;
                dutyPm = 500u;
            } else if (phase == 3) {
                setHz = 2000u;
                dutyPm = 250u;
            } else {
                setHz = 1000u + (mvR38 * 4000u) / 3300u;  // 1 kHz .. 5 kHz
                dutyPm = (mvR37 * 1000u) / 3300u;         // 0 .. 1000 permille
                if (dutyPm > 1000u) dutyPm = 1000u;
            }
            uint32_t arr = (1000000u / setHz);
            if (arr < 2u) arr = 2u;
            arr -= 1u;                                    // counter ticks - 1
            uint32_t ccr = ((arr + 1u) * dutyPm + 500u) / 1000u;
            if (ccr > arr) ccr = arr;
            if (phase != 1) pwm_set(arr, ccr);            // phase 1 keeps its config

            // what the timer really produces now
            realHz = 1000000u / (TIM_ARR(TIM3) + 1u);
            realDutyPm = (TIM_CCR2(TIM3) * 1000u) / (TIM_ARR(TIM3) + 1u);
            MAIL_PA7_HZ = realHz;
            MAIL_PA7_DUTY = realDutyPm;

            // measured input frequencies (PA15 capture, PB4 capture)
            pb4Hz = g_pb4_hz;
            MAIL_PA15_HZ = g_pa15_hz;
            MAIL_DELTA = g_cap_delta;
            MAIL_OVF = g_ovf;
            MAIL_PB4_HZ = pb4Hz;
            MAIL_PB4_DELTA = g_pb4_delta;
            MAIL_PB4_OVF = g_pb4_ovf;

            // firmware's own plausibility check (never the simulator)
            const uint32_t diff = (g_pa15_hz > realHz) ? (g_pa15_hz - realHz)
                                                      : (realHz - g_pa15_hz);
            alarm = (phase == 1) ? 0u : ((diff > 1000u) ? 1u : 0u);
            MAIL_ALARM = alarm;

            // LED: alarm -> all on, otherwise a 0.5 s heartbeat on LD1
            if (alarm) {
                led_disp(0xFFu);
            } else if (prevAlarm) {
                // alarm just cleared: drop back to the heartbeat immediately
                lastLedMs = g_uwTick;
                ledOn = 0;
                led_disp(ledOn);
            } else if ((g_uwTick - lastLedMs) >= 500u) {
                lastLedMs = g_uwTick;
                ledOn ^= 1u;
                led_disp(ledOn);
            }
            prevAlarm = alarm;
        }

        // ---- LCD refresh (interrupts masked: the ISR shares GPIOC) ----
        if ((g_uwTick - lastLcdMs) >= 500u) {
            lastLcdMs = g_uwTick;
            __asm volatile("cpsid i");
            lcd_show_phase(phase, mvR37, mvR38, g_pa15_hz, pb4Hz, realHz,
                           realDutyPm, alarm);
            __asm volatile("cpsie i");
        }
    }
}