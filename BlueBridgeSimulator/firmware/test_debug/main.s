/* CT117E-M4 debug-target test firmware (stage 7-1).
 *
 * Pure Thumb assembly, no HAL, no interrupts, no SysTick, no LCD/USART: its
 * only job is to give tests/debug_target_selftest.cpp a completely
 * deterministic instruction stream and a fixed set of addresses to break on.
 *
 * Instruction layout (fixed; validated at run time through the .debugmap
 * magic -- see the self test):
 *
 *   Reset_Handler:  ldr r1, =0x20000100
 *                   nop
 *   step_1:         movs r0, #1
 *   step_2:         adds r0, r0, #1
 *   step_3:         adds r0, r0, #1
 *   step_4:         str  r0, [r1]        -> SRAM[0x20000100] = r0
 *   loop_top:       adds r2, r2, #1
 *                   str  r2, [r1, #4]    -> SRAM[0x20000104] = loop count
 *                   b    loop_top
 *
 * The .debugmap section is placed at a FIXED flash address (0x08000200) by
 * linker.ld, so the self test can read every label address BEFORE a single
 * instruction has executed (i.e. right after reset-halt).
 */

    .syntax unified
    .thumb

/* ---- vector table (16 entries: SP, Reset, then Default_Handler) ---------- */
    .section .isr_vector, "a", %progbits
    .align 2
    .global g_vectors
g_vectors:
    .word _estack
    .word Reset_Handler
    .rept 14
    .word Default_Handler
    .endr

/* ---- code ---------------------------------------------------------------- */
    .section .text, "ax", %progbits
    .align 2

    .global Reset_Handler
    .thumb_func
Reset_Handler:
    ldr r1, =0x20000100
    nop

    .global step_1
    .thumb_func
step_1:
    movs r0, #1

    .global step_2
    .thumb_func
step_2:
    adds r0, r0, #1

    .global step_3
    .thumb_func
step_3:
    adds r0, r0, #1

    .global step_4
    .thumb_func
step_4:
    str r0, [r1]

    .global loop_top
    .thumb_func
loop_top:
    adds r2, r2, #1
    str r2, [r1, #4]
    b loop_top

    .thumb_func
Default_Handler:
    b Default_Handler

    .align 2
    .ltorg

/* ---- debug map: fixed address 0x08000200 (see linker.ld) -----------------
 * [0] magic        0xDEB06001
 * [1] Reset_Handler
 * [2] step_1       [3] step_2       [4] step_3      [5] step_4
 * [6] loop_top
 * [7] scratch address used by step_4  (0x20000100)
 * [8] loop counter address           (0x20000104)
 * [9] initial SP (_estack)
 * [10..14] reserved
 * [15] end marker 0xDEB06E0D
 */
    .section .debugmap, "a", %progbits
    .align 2
    .word 0xDEB06001
    .word Reset_Handler
    .word step_1
    .word step_2
    .word step_3
    .word step_4
    .word loop_top
    .word 0x20000100
    .word 0x20000104
    .word _estack
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0xDEB06E0D