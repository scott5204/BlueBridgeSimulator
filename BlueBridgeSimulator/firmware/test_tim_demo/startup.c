// Minimal Cortex-M4 startup for the TIM demo firmware.
// STM32G431RBT6: flash 0x08000000 (128K), SRAM 0x20000000 (22K).
//
// Vector table large enough for the timers used here:
//   TIM2_IRQn = 28 -> vector 16 + 28 = 44  (0.5 s blink interrupt)
#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sselfdata, _eselfdata, _siselfdata;
extern uint32_t _sdata, _edata, _sidata;
extern uint32_t _sbss, _ebss;

int main(void);
void SysTick_Handler(void);
void TIM2_IRQHandler(void);
void Reset_Handler(void);
void Default_Handler(void);

// Vector table: [0]=SP [1]=Reset ... [15]=SysTick ... [44]=TIM2
__attribute__((section(".isr_vector"), used))
const void* g_vectors[16 + 32] = {
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
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 16-27 IRQ0..11
    0, 0, 0, 0,          // 28-31 IRQ12..15
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 32-43 IRQ16..27
    TIM2_IRQHandler,     // 44 IRQ28 = TIM2
    Default_Handler,     // 45 IRQ29 = TIM3 (PWM only, no interrupt here)
    0, 0,                // 46-47 IRQ30..31
};

void Reset_Handler(void) {
    // copy .selfdata (self-test mailbox at 0x20000000)
    {
        uint32_t* src = &_siselfdata;
        uint32_t* dst = &_sselfdata;
        while (dst < &_eselfdata) *dst++ = *src++;
    }
    // copy .data
    {
        uint32_t* src = &_sidata;
        uint32_t* dst = &_sdata;
        while (dst < &_edata) *dst++ = *src++;
    }
    // zero .bss
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