// Minimal Cortex-M4 startup for the USART1 test firmware.
// STM32G431RBT6: flash 0x08000000 (128K), SRAM 0x20000000 (22K).
//
// Vectors used:
//   SysTick  = 15          (1 kHz time base)
//   USART1   = 16 + 37 = 53  (RXNE interrupt)
#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sselfdata, _eselfdata, _siselfdata;
extern uint32_t _sdata, _edata, _sidata;
extern uint32_t _sbss, _ebss;

int main(void);
void SysTick_Handler(void);
void USART1_IRQHandler(void);
void Reset_Handler(void);
void Default_Handler(void);

__attribute__((section(".isr_vector"), used))
const void* g_vectors[16 + 40] = {
    (void*)&_estack,     // 0 initial SP
    Reset_Handler,       // 1
    Default_Handler,     // 2 NMI
    Default_Handler,     // 3 HardFault
    Default_Handler,     // 4 MemManage
    Default_Handler,     // 5 BusFault
    Default_Handler,     // 6 UsageFault
    0, 0, 0, 0,          // 7-10 reserved
    Default_Handler,     // 11 SVCall
    Default_Handler,     // 12 DebugMonitor
    0,                   // 13 reserved
    Default_Handler,     // 14 PendSV
    SysTick_Handler,     // 15 SysTick
    0, 0, 0, 0, 0, 0, 0, 0,   // 16-23 IRQ0..7
    0, 0, 0, 0, 0, 0, 0, 0,   // 24-31 IRQ8..15
    0, 0, 0, 0, 0, 0, 0, 0,   // 32-39 IRQ16..23
    0, 0, 0, 0, 0, 0, 0, 0,   // 40-47 IRQ24..31
    0, 0, 0, 0, 0,            // 48-52 IRQ32..36
    USART1_IRQHandler,        // 53 IRQ37 = USART1
    0, 0,                     // 54-55 IRQ38..39
};

void Reset_Handler(void) {
    {
        uint32_t* src = &_siselfdata;
        uint32_t* dst = &_sselfdata;
        while (dst < &_eselfdata) *dst++ = *src++;
    }
    {
        uint32_t* src = &_sidata;
        uint32_t* dst = &_sdata;
        while (dst < &_edata) *dst++ = *src++;
    }
    {
        uint32_t* dst = &_sbss;
        while (dst < &_ebss) *dst++ = 0;
    }
    main();
    for (;;) {}
}

void Default_Handler(void) {
    for (;;) {}
}