// CT117E-M4 USART1 firmware (bare metal, direct MMIO) -- stage 5.
//
// Everything goes through real memory-mapped registers:
//   * USART1 on PA9 (TX, AF7) / PA10 (RX, AF7) -- the DAP-Link USB serial
//     port, 9600 8N1 decoded from BRR (0x208D = 8333 cycles per bit at
//     80 MHz PCLK2 -> 1.0416 ms per byte)
//   * a real USART1_IRQHandler (RXNEIE + NVIC IRQ 37) receives the lines and
//     counts its interrupt entries in the mailbox, so the self test can prove
//     the NVIC delivered a real exception
//   * line protocol: "(x,y)" -> "Got it", "?" -> "Idle", "#" -> "(x,y)",
//     anything else is echoed back -- the 15th competition style protocol,
//     implemented entirely in firmware (the simulator only moves bytes)
//
// Test phases, driven by the mailbox command word [1]:
//   0  default: HELLO at boot, then the line protocol (echo everything else)
//   1  TX burst timing: 100 bytes at 9600 (expect ~104 ms of virtual time)
//   2  wrong TX AF: PA9 left as an input, "HELLO" again -> 0 bytes on the wire
//   3  wrong RX AF: PA10 left as an input, host bytes must not set RXNE
//   4  overrun: 'A' unread + 'B' -> ORE=1, cleared through ICR
//   5  (reserved)
//   6  (reserved)
//   7  baud test: BRR switched to 115200, 100 bytes (expect ~9 ms)
//
// Mailbox at 0x20000000 (.selfdata, magic 0xC0FFEE05) -- see usart_selftest.cpp
// for the field list.
#include "main.h"

__attribute__((section(".selfdata"), used))
volatile uint32_t g_mailbox[20] = {
    0xC0FFEE05u,  // [0]  magic
    0,            // [1]  cmd (host -> firmware)
    0,            // [2]  phase (firmware -> host)
    0,            // [3]  TX bytes handed to USART1 (firmware counter)
    0,            // [4]  RX bytes received (counted in the ISR)
    0,            // [5]  RXNE interrupt entries (ISR count)
    0,            // [6]  last RX byte
    0,            // [7]  BRR value in effect
    0,            // [8]  100-byte burst time at 9600 [ms]
    0,            // [9]  ORE observed after the second byte (expect 1)
    0,            // [10] ORE after ICR.ORECF (expect 0)
    0,            // [11] RDR read after the overrun (expect 'A' = 0x41)
    0,            // [12] protocol replies sent
    0,            // [13] last parsed x
    0,            // [14] last parsed y
    0,            // [15] main loop count
    0,            // [16] TXE wait timeouts (must stay 0)
    0,            // [17] 100-byte burst time at 115200 [ms]
    0,            // [18] RX byte count when the last phase was entered
    0,            // [19] reserved
};

#define MAIL_CMD (g_mailbox[1])
#define MAIL_PHASE (g_mailbox[2])
#define MAIL_TX_BYTES (g_mailbox[3])
#define MAIL_RX_BYTES (g_mailbox[4])
#define MAIL_RX_IRQ (g_mailbox[5])
#define MAIL_LAST_BYTE (g_mailbox[6])
#define MAIL_BRR (g_mailbox[7])
#define MAIL_BURST_9600 (g_mailbox[8])
#define MAIL_ORE_SEEN (g_mailbox[9])
#define MAIL_ORE_CLEARED (g_mailbox[10])
#define MAIL_ORE_RDR (g_mailbox[11])
#define MAIL_REPLIES (g_mailbox[12])
#define MAIL_X (g_mailbox[13])
#define MAIL_Y (g_mailbox[14])
#define MAIL_LOOP (g_mailbox[15])
#define MAIL_TXE_TIMEOUTS (g_mailbox[16])
#define MAIL_BURST_115200 (g_mailbox[17])
#define MAIL_RX_AT_PHASE (g_mailbox[18])

volatile uint32_t g_uwTick;   // 1 kHz SysTick counter
static volatile uint32_t s_tx_bytes;   // bytes handed to the transmitter
static uint8_t s_line[64];
static uint32_t s_line_len;
static uint32_t s_x = 0, s_y = 0;
static uint32_t s_led_on = 0;

void SysTick_Handler(void) { g_uwTick++; }

// ---------------------------------------------------------------------------
// USART1 interrupt: the whole receive path (RXNEIE), like a real project
// ---------------------------------------------------------------------------
static void line_byte(uint8_t b);

void USART1_IRQHandler(void) {
    const uint32_t isr = USART1->ISR;
    if (isr & USART_ISR_RXNE) {
        const uint8_t b = (uint8_t)(USART1->RDR & 0xFFu);
        MAIL_RX_IRQ++;
        MAIL_LAST_BYTE = b;
        MAIL_RX_BYTES++;
        line_byte(b);
        return;
    }
    if (isr & USART_ISR_ORE) {
        USART1->ICR = USART_ICR_ORECF;  // release the IRQ line
    }
    if (isr & USART_ISR_TC) {
        USART1->ICR = USART_ICR_TCCF;
    }
}

// ---------------------------------------------------------------------------
// transmitter (polling TXE, exactly what the competition code does)
// ---------------------------------------------------------------------------
static void uart_tx_byte(uint8_t b) {
    uint32_t guard = 0;
    while ((USART1->ISR & USART_ISR_TXE) == 0) {
        if (++guard > 10000000u) {
            MAIL_TXE_TIMEOUTS++;
            return;
        }
    }
    USART1->TDR = b;
    s_tx_bytes++;
    MAIL_TX_BYTES = s_tx_bytes;  // live progress for the self test
}

static void uart_tx(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) uart_tx_byte(p[i]);
}

static void uart_tx_str(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    uart_tx((const uint8_t*)s, n);
}

static void uart_tx_u32(uint32_t v) {
    char buf[12];
    uint32_t n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[12];
        uint32_t t = 0;
        while (v > 0) {
            tmp[t++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (t > 0) buf[n++] = tmp[--t];
    }
    uart_tx((const uint8_t*)buf, n);
}

// ---------------------------------------------------------------------------
// line protocol (15th competition style)
// ---------------------------------------------------------------------------
static int parse_xy(const uint8_t* s, uint32_t len, uint32_t* x, uint32_t* y) {
    uint32_t i = 0;
    if (len < 5 || s[i++] != '(') return 0;
    uint32_t vx = 0, vy = 0;
    uint32_t digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        vx = vx * 10u + (uint32_t)(s[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0 || i >= len || s[i++] != ',') return 0;
    digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        vy = vy * 10u + (uint32_t)(s[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0 || i >= len || s[i++] != ')') return 0;
    if (i != len) return 0;
    *x = vx;
    *y = vy;
    return 1;
}

static void process_line(void) {
    static const uint8_t got_it[] = "Got it\r\n";
    static const uint8_t idle[] = "Idle\r\n";
    if (s_line_len == 0) return;
    if (s_line[0] == '(') {
        uint32_t x, y;
        if (parse_xy(s_line, s_line_len, &x, &y)) {
            s_x = x;
            s_y = y;
            MAIL_X = x;
            MAIL_Y = y;
            uart_tx(got_it, sizeof(got_it) - 1);
            MAIL_REPLIES++;
        } else {
            uart_tx((const uint8_t*)"Err\r\n", 5);
        }
        return;
    }
    if (s_line_len == 1 && s_line[0] == '?') {
        uart_tx(idle, sizeof(idle) - 1);
        MAIL_REPLIES++;
        return;
    }
    if (s_line_len == 1 && s_line[0] == '#') {
        uart_tx((const uint8_t*)"(", 1);
        uart_tx_u32(s_x);
        uart_tx((const uint8_t*)",", 1);
        uart_tx_u32(s_y);
        uart_tx((const uint8_t*)")\r\n", 3);
        MAIL_REPLIES++;
        return;
    }
    // anything else: echo the line back
    uart_tx(s_line, s_line_len);
    uart_tx((const uint8_t*)"\r\n", 2);
}

static void line_byte(uint8_t b) {
    if (b == '\r' || b == '\n') {
        if (s_line_len) {
            process_line();
            s_line_len = 0;
        }
        return;
    }
    if (s_line_len < sizeof(s_line) - 1) s_line[s_line_len++] = b;
    if (s_line_len == sizeof(s_line) - 1) {  // full buffer: process it
        process_line();
        s_line_len = 0;
    }
}

// ---------------------------------------------------------------------------
// GPIO / clock / time base
// ---------------------------------------------------------------------------
static void clock_init(void) {
    RCC_CR |= (1u << 16);                 // HSEON
    while ((RCC_CR & (1u << 17)) == 0) {}  // HSERDY
    RCC_PLLCFGR = (3u << 0) | (2u << 4) | (20u << 8) | (1u << 24);
    RCC_CR |= (1u << 24);                  // PLLON
    while ((RCC_CR & (1u << 25)) == 0) {}  // PLLRDY
    FLASH_ACR = 2u;
    RCC_CFGR = (RCC_CFGR & ~3u) | 3u;      // SYSCLK = PLL (80 MHz)
    while (((RCC_CFGR >> 2) & 3u) != 3u) {}
}

static void systick_init(void) {
    SYST_RVR = 79999u;
    SYST_CVR = 0;
    SYST_CSR = 7u;
}

static void gpio_af(int port, int pin, uint32_t af) {
    GPIO_TypeDef* g = port == 0 ? GPIOA : (port == 1 ? GPIOB : GPIOC);
    g->MODER = (g->MODER & ~(3u << (pin * 2))) | (2u << (pin * 2));
    g->OTYPER &= ~(1u << pin);
    if (pin < 8)
        g->AFR[0] = (g->AFR[0] & ~(0xFu << (pin * 4))) | (af << (pin * 4));
    else
        g->AFR[1] =
            (g->AFR[1] & ~(0xFu << ((pin - 8) * 4))) | (af << ((pin - 8) * 4));
}

static void gpio_input(int port, int pin) {
    GPIO_TypeDef* g = port == 0 ? GPIOA : (port == 1 ? GPIOB : GPIOC);
    g->MODER &= ~(3u << (pin * 2));  // input (no AF: USART1 sees nothing)
}

static void led_disp(uint32_t pattern);

static void led_init(void) {
    RCC_AHB2ENR |= (1u << 2) | (1u << 3);  // GPIOC (LED latch data), GPIOD (LE)
    (void)RCC_AHB2ENR;
    GPIOD->MODER = (GPIOD->MODER & ~(3u << (2 * 2))) | (1u << (2 * 2));
    GPIOD->BSRR = (1u << 2) << 16;
    led_disp(0);
}

static void led_disp(uint32_t pattern) {
    uint32_t odr = GPIOC->ODR & 0x00FFu;
    odr |= (~pattern & 0xFFu) << 8;
    GPIOC->ODR = odr;
    GPIOD->BSRR = (1u << 2);
    GPIOD->BSRR = (1u << 2) << 16;
}

// ---------------------------------------------------------------------------
// USART1 setup (the "CubeMX generated" part, written as registers)
// ---------------------------------------------------------------------------
static void usart1_default_config(uint32_t brr) {
    RCC_APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC_APB2ENR;
    RCC_AHB2ENR |= (1u << 0);  // GPIOA clock (writes are dropped without it!)
    (void)RCC_AHB2ENR;
    gpio_af(0, 9, 7);   // PA9  = USART1_TX AF7
    gpio_af(0, 10, 7);  // PA10 = USART1_RX AF7
    USART1->CR1 = 0;    // configure while disabled
    USART1->BRR = brr;
    USART1->CR2 = 0;    // 1 stop bit, no clock
    USART1->CR3 = 0;
    USART1->PRESC = 0;  // input clock / 1
    USART1->ICR = 0xFFFFFFFFu;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_ISER1 = (1u << (USART1_IRQN - 32));  // USART1 global interrupt
}

// ---------------------------------------------------------------------------
// test phases (host driven through the mailbox)
// ---------------------------------------------------------------------------
static uint32_t burst_100(uint8_t fill) {
    uint8_t buf[100];
    for (uint32_t i = 0; i < 100; i++) buf[i] = fill;
    const uint32_t t0 = g_uwTick;
    uart_tx(buf, 100);
    MAIL_TX_BYTES = s_tx_bytes;
    return g_uwTick - t0;
}

static void spin_ms(uint32_t ms) {
    const uint32_t t0 = g_uwTick;
    while ((g_uwTick - t0) < ms) {}
}

static uint32_t apply_phase(uint32_t phase) {
    MAIL_RX_AT_PHASE = MAIL_RX_BYTES;
    MAIL_PHASE = phase;  // visible to the host before the blocking work below
    switch (phase) {
    case 1: {  // TX burst timing at 9600 (must NOT finish instantly)
        usart1_default_config(USART_BRR_9600);
        MAIL_BRR = USART1->BRR;
        MAIL_BURST_9600 = burst_100('T');
        break;
    }
    case 2: {  // wrong TX AF: PA9 is not routed -> the terminal sees 0 bytes
        usart1_default_config(USART_BRR_9600);
        gpio_input(0, 9);
        uart_tx_str("HELLO\r\n");
        break;
    }
    case 3: {  // wrong RX AF: PA10 is not routed -> RXNE must stay 0
        usart1_default_config(USART_BRR_9600);
        gpio_input(0, 10);
        break;
    }
    case 4: {  // overrun: send 'A' (unread), then 'B'
        usart1_default_config(USART_BRR_9600);
        USART1->CR1 &= ~USART_CR1_RXNEIE;  // the ISR must stay silent here
        if (USART1->ISR & USART_ISR_RXNE) (void)USART1->RDR;
        USART1->ICR = USART_ICR_ORECF;
        MAIL_ORE_SEEN = 0;
        MAIL_ORE_CLEARED = 0;
        MAIL_ORE_RDR = 0;
        uint32_t tries = 0;
        while (!(USART1->ISR & USART_ISR_RXNE) && tries++ < 400) spin_ms(1);
        tries = 0;
        while (!(USART1->ISR & USART_ISR_ORE) && tries++ < 400) spin_ms(1);
        MAIL_ORE_SEEN = (USART1->ISR & USART_ISR_ORE) ? 1u : 0u;
        USART1->ICR = USART_ICR_ORECF;   // the documented clear path
        MAIL_ORE_CLEARED = (USART1->ISR & USART_ISR_ORE) ? 1u : 0u;
        MAIL_ORE_RDR = (uint32_t)(USART1->RDR & 0xFFu);  // the unread 'A'
        USART1->CR1 |= USART_CR1_RXNEIE;
        break;
    }
    case 7: {  // baud test: 115200 comes straight from BRR
        usart1_default_config(USART_BRR_115200);
        MAIL_BRR = USART1->BRR;
        MAIL_BURST_115200 = burst_100('U');
        break;
    }
    default: {  // 0 / unknown: the normal protocol configuration
        usart1_default_config(USART_BRR_9600);
        MAIL_BRR = USART1->BRR;
        phase = 0;
        break;
    }
    }
    return phase;
}

int main(void) {
    SCB_CPACR = (0xFu << 20);
    __asm volatile("dsb");
    __asm volatile("isb");

    clock_init();
    systick_init();
    led_init();

    usart1_default_config(USART_BRR_9600);
    MAIL_BRR = USART1->BRR;
    uart_tx_str("HELLO\r\n");  // test 1: the very first transmission
    MAIL_TX_BYTES = s_tx_bytes;
    MAIL_PHASE = 0;

    uint32_t phase = 0;
    uint32_t lastLed = 0;
    for (;;) {
        MAIL_LOOP++;

        const uint32_t cmd = MAIL_CMD;
        if (cmd != phase) {
            phase = apply_phase(cmd);
            MAIL_PHASE = phase;
        }

        MAIL_TX_BYTES = s_tx_bytes;

        // heartbeat on LD1 so the board view stays alive
        if ((g_uwTick - lastLed) >= 500u) {
            lastLed = g_uwTick;
            s_led_on = !s_led_on;
            led_disp(s_led_on ? 0x01u : 0u);
        }
    }
}