// CT117E-M4 LCD test firmware.
//
// Runs the OFFICIAL CT117E LCD BSP (lcd.c / lcd.h / fonts.h, unmodified
// except for the HAL subset provided by hal_shim.c) on the emulated
// Cortex-M4. Everything the panel shows must travel the real path:
//
//   ARM instructions -> GPIOA/B/C registers -> CT117E-M4 board wiring
//                    -> LCD parallel bus -> ILI9325 controller -> GRAM
//
// Nothing in this file or in the BSP draws text/pixels on the host side.
//
// Observable behaviour (mailbox at 0x20000000, .selfdata):
//   [0] magic 0x1CD00001   [1] uwTick (ms, SysTick 1 kHz)
//   [2] main loop count    [3] stage: 1=init 2=clear 3=text 4=blocks done
//   [4] controller code read back over the bus (LCD_ReadReg(0))
//   [5] RCC_CFGR after the 80 MHz PLL switch
#include <stdint.h>

#include "lcd.h"  // official BSP (includes main.h)

__attribute__((section(".selfdata"), used))
volatile uint32_t g_mailbox[6] = {
    0x1CD00001u,  // magic
    0,            // uwTick
    0,            // loop count
    0,            // stage
    0,            // controller device code (read over the bus)
    0,            // RCC_CFGR
};

#define MAIL_TICK (g_mailbox[1])
#define MAIL_LOOP (g_mailbox[2])
#define MAIL_STAGE (g_mailbox[3])
#define MAIL_DEVID (g_mailbox[4])
#define MAIL_CFGR (g_mailbox[5])

// lcd.c global: last result of LCD_ReadReg() (controller detection)
extern vu16 dummy;

// ---- RCC / SysTick registers (STM32G431, RM0440/CMSIS) ----
#define RCC_BASE 0x40021000u
#define RCC_CR (*(volatile uint32_t*)(RCC_BASE + 0x00u))
#define RCC_CFGR (*(volatile uint32_t*)(RCC_BASE + 0x08u))
#define RCC_PLLCFGR (*(volatile uint32_t*)(RCC_BASE + 0x0Cu))

#define FLASH_ACR (*(volatile uint32_t*)0x40022000u)

#define SYST_CSR (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018u)

#define SCB_CPACR (*(volatile uint32_t*)0xE000ED88u)

void SysTick_Handler(void) {
    g_uwTick++;
}

static void clock_init(void) {
    // HSE 24 MHz on CT117E-M4
    RCC_CR |= (1u << 16);
    while ((RCC_CR & (1u << 17)) == 0) {
    }

    // PLL: HSE /M=3 *N=20 /R=2 -> 80 MHz
    RCC_PLLCFGR = (3u << 0) | (2u << 4) | (20u << 8) | (1u << 24);
    RCC_CR |= (1u << 24);
    while ((RCC_CR & (1u << 25)) == 0) {
    }

    FLASH_ACR = 2u;  // 2 wait states

    RCC_CFGR = (RCC_CFGR & ~3u) | 3u;  // SYSCLK <- PLL
    while (((RCC_CFGR >> 2) & 3u) != 3u) {
    }
}

static void systick_init(void) {
    SYST_RVR = 79999u;  // 80 MHz / 1 kHz - 1
    SYST_CVR = 0;
    SYST_CSR = 7u;  // ENABLE | TICKINT | CLKSOURCE
}

// Fill a rectangle given in PANEL (landscape 320x240) coordinates using the
// BSP primitives. GRAM mapping of the module: px = 319 - gramY, py = gramX,
// and the address counter runs down Y (R03h = 0x1018, AM=1 I/D=01), so one
// LCD_SetCursor + w consecutive LCD_WriteRAM fill px..px+w-1 of a panel row.
static void lcd_fill_panel_rect(uint16_t px, uint16_t py, uint16_t w,
                                uint16_t h, u16 color) {
    for (uint16_t i = 0; i < h; i++) {
        LCD_SetCursor((u8)(py + i), (u16)(319 - px));
        LCD_WriteRAM_Prepare();
        for (uint16_t j = 0; j < w; j++) {
            LCD_WriteRAM(color);
        }
    }
}

int main(void) {
    // FPU on (the BSP/font code is compiled hard-float)
    SCB_CPACR = (0xFu << 20);
    __asm volatile("dsb");
    __asm volatile("isb");

    clock_init();
    systick_init();
    MAIL_CFGR = RCC_CFGR;

    // ---- official BSP init (controller detection through the bus) ----
    LCD_Init();
    MAIL_DEVID = dummy;  // value the controller drove on GPIOC->IDR
    MAIL_STAGE = 1;

    // ---- clear the whole screen through 76800 GRAM pixel writes ----
    LCD_Clear(Black);
    MAIL_STAGE = 2;

    // ---- text: white "BlueBridge" on line 1, red "LCD TEST" on line 2 ----
    LCD_SetBackColor(Black);
    LCD_SetTextColor(White);
    LCD_DisplayStringLine(Line1, (u8*)"BlueBridge");
    LCD_SetTextColor(Red);
    LCD_DisplayStringLine(Line2, (u8*)"LCD TEST");
    MAIL_STAGE = 3;

    // ---- colour blocks (40x24 each, panel row 100..123) ----
    lcd_fill_panel_rect(40, 100, 40, 24, Red);
    lcd_fill_panel_rect(100, 100, 40, 24, Green);
    lcd_fill_panel_rect(160, 100, 40, 24, Blue);
    lcd_fill_panel_rect(220, 100, 40, 24, White);
    lcd_fill_panel_rect(280, 100, 40, 24, Cyan);
    MAIL_STAGE = 4;

    for (;;) {
        MAIL_LOOP++;
        MAIL_TICK = g_uwTick;
    }
}