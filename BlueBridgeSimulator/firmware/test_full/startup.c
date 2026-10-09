// Minimal Cortex-M4 startup for the ADC/PWM closed-loop firmware.
// STM32G431RBT6: flash 0x08000000 (128K), SRAM 0x20000000 (22K).
//
// Vectors used:
//   SysTick  = 15          (1 kHz time base)
//   ADC1_2   = 16 + 18 = 34
//   TIM16    = 16 + 25 = 41  (PB4 input capture)
//   TIM2     = 16 + 28 = 44  (PA15 input capture)
//   TIM3     = 16 + 29 = 45  (PA7 PWM, update interrupt)
#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sselfdata, _eselfdata, _siselfdata;
extern uint32_t _sdata, _edata, _sidata;
extern uint32_t _sbss, _ebss;

int main(void);
void SysTick_Handler(void);
void ADC1_2_IRQHandler(void);
void TIM16_IRQHandler(void);
void TIM2_IRQHandler(void);
void TIM3_IRQHandler(void);
void Reset_Handler(void);
void Default_Handler(void);

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
    0, 0,                // 32-33 IRQ16/17
    ADC1_2_IRQHandler,   // 34 IRQ18 = ADC1_2
    0, 0, 0, 0, 0, 0,                     // 35-40
    TIM16_IRQHandler,    // 41 IRQ25 = TIM16 (PB4 capture)
    0, 0,                // 42-43
    TIM2_IRQHandler,     // 44 IRQ28 = TIM2
    TIM3_IRQHandler,     // 45 IRQ29 = TIM3
    0, 0,                // 46-47
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