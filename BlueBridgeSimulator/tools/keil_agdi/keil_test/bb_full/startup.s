; bb_full -- CT117E-M4 full self-test firmware, Keil AC6 (ArmAsm) startup.
;
; Keil-side twin of firmware/test_full/startup.c: the SAME 48-entry vector
; table (16 core + 32 IRQ) and the SAME static heap/stack override, so the
; two builds of the self-test behave identically in the simulator.
;
; Vectors used by main.c (STM32G431):
;   SysTick  = 15           1 kHz time base (g_uwTick / key debounce)
;   ADC1_2   = 16 + 18 = 34 ADC1/ADC2 end of conversion
;   TIM16    = 16 + 25 = 41 PB4 input capture
;   TIM2     = 16 + 28 = 44 PA15 input capture
;   TIM3     = 16 + 29 = 45 PA7 PWM update
; Every other IRQ stays on Default_Handler (stop in the debugger if one ever
; fires, instead of running with a NULL vector).
;
; Reset_Handler hands over to the C runtime (__main): the scatter file
; bb_full.sct places .selfdata at 0x20000000, ER_IROM1 holds the image and
; __main performs the RW/ZI initialization from it.

Stack_Size      EQU     0x00000400

                AREA    STACK, NOINIT, READWRITE, ALIGN=3
Stack_Mem       SPACE   Stack_Size
__initial_sp

                AREA    RESET, DATA, READONLY, ALIGN=2
                EXPORT  __Vectors
                EXPORT  __Vectors_End
                EXPORT  __Vectors_Size

                IMPORT  SysTick_Handler
                IMPORT  ADC1_2_IRQHandler
                IMPORT  TIM16_IRQHandler
                IMPORT  TIM2_IRQHandler
                IMPORT  TIM3_IRQHandler

__Vectors       DCD     __initial_sp              ; 0x00  initial SP
                DCD     Reset_Handler             ; 0x04  reset
                DCD     Default_Handler           ; 0x08  NMI
                DCD     Default_Handler           ; 0x0C  HardFault
                DCD     Default_Handler           ; 0x10  MemManage
                DCD     Default_Handler           ; 0x14  BusFault
                DCD     Default_Handler           ; 0x18  UsageFault
                DCD     0, 0, 0, 0                ; 0x1C..0x28 reserved
                DCD     Default_Handler           ; 0x2C  SVCall
                DCD     Default_Handler           ; 0x30  DebugMonitor
                DCD     0                         ; 0x34  reserved
                DCD     Default_Handler           ; 0x38  PendSV
                DCD     SysTick_Handler           ; 0x3C  SysTick (15)
                ; IRQ0..17  (vectors 16..33)
                DCD     Default_Handler, Default_Handler, Default_Handler, Default_Handler
                DCD     Default_Handler, Default_Handler, Default_Handler, Default_Handler
                DCD     Default_Handler, Default_Handler, Default_Handler, Default_Handler
                DCD     Default_Handler, Default_Handler, Default_Handler, Default_Handler
                DCD     Default_Handler, Default_Handler
                ; IRQ18..29
                DCD     ADC1_2_IRQHandler         ; 34  ADC1_2  (IRQ18)
                DCD     Default_Handler, Default_Handler, Default_Handler, Default_Handler
                DCD     Default_Handler, Default_Handler
                DCD     TIM16_IRQHandler          ; 41  TIM16   (IRQ25, PB4 capture)
                DCD     Default_Handler, Default_Handler
                DCD     TIM2_IRQHandler           ; 44  TIM2    (IRQ28, PA15 capture)
                DCD     TIM3_IRQHandler           ; 45  TIM3    (IRQ29, PA7 PWM)
                DCD     Default_Handler, Default_Handler
__Vectors_End

__Vectors_Size  EQU     __Vectors_End - __Vectors

                AREA    |.text|, CODE, READONLY, ALIGN=2
                THUMB
                PRESERVE8

Reset_Handler   PROC
                EXPORT  Reset_Handler             [WEAK]
                IMPORT  __main
                LDR     R0, =__main
                BX      R0
                ENDP

Default_Handler PROC
                EXPORT  Default_Handler           [WEAK]
                B       .
                ENDP

; Static heap/stack setup: overrides the C library's default
; __user_initial_stackheap, which asks the debugger for heap info through
; semihosting (BKPT 0xAB / SYS_HEAPINFO). The simulator has no semihosting
; channel, so the default version faults into Default_Handler before main and
; every run/breakpoint test would hang. Layout (32 KiB SRAM at 0x20000000):
;   heap  0x20000100 .. 0x20004000
;   stack limit 0x20006000, stack base (top) 0x20008000
__user_initial_stackheap PROC
                EXPORT  __user_initial_stackheap
                LDR     R0, =0x20000100           ; heap base
                LDR     R1, =0x20004000           ; heap limit
                LDR     R2, =0x20008000           ; stack base
                LDR     R3, =0x20006000           ; stack limit
                BX      LR
                ENDP

                ALIGN
                END