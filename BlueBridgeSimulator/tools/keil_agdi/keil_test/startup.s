; B.3 Keil AGDI test fixture -- minimal Cortex-M4 startup (ArmAsm syntax, AC6).
;
; Only the 16 Cortex-M core exception vectors are needed: the test firmware
; enables no interrupts and uses no system tick. Reset_Handler hands over to the
; C runtime exactly like the DFP startup files do (__main -> main), so the
; scatter file generated from the "Target" dialog (0x08000000 / 0x20000000) is
; all the memory layout this fixture needs.

Stack_Size      EQU     0x00000400

                AREA    STACK, NOINIT, READWRITE, ALIGN=3
Stack_Mem       SPACE   Stack_Size
__initial_sp

                AREA    RESET, DATA, READONLY, ALIGN=2
                EXPORT  __Vectors
                EXPORT  __Vectors_End
                EXPORT  __Vectors_Size

__Vectors       DCD     __initial_sp              ; 0x00  initial SP
                DCD     Reset_Handler             ; 0x04  reset
                DCD     Default_Handler           ; 0x08  NMI
                DCD     Default_Handler           ; 0x0C  HardFault
                DCD     Default_Handler           ; 0x10  MemManage
                DCD     Default_Handler           ; 0x14  BusFault
                DCD     Default_Handler           ; 0x18  UsageFault
                DCD     0                         ; 0x1C
                DCD     0                         ; 0x20
                DCD     0                         ; 0x24
                DCD     0                         ; 0x28
                DCD     Default_Handler           ; 0x2C  SVC
                DCD     Default_Handler           ; 0x30  DebugMon
                DCD     0                         ; 0x34
                DCD     Default_Handler           ; 0x38  PendSV
                DCD     Default_Handler           ; 0x3C  SysTick
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