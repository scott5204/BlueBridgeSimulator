/* CT117E-M4 virtual-flash programming test firmware -- build A (stage 7-2A).
 *
 * Build A and build B share the SAME linker layout: every instruction sits at
 * the same flash address in both images. They differ in exactly two places:
 *
 *   1. the MOVS immediate at `program_code` (fixed address, see the map below)
 *      A: movs r0, #0x11   -> SRAM[0x20000204] = 0x00000011
 *      B: movs r0, #0x22   -> SRAM[0x20000204] = 0x00000022
 *      This is a real OPCODE difference at the same virtual address -- it is
 *      what catches a stale translation block: after PROGRAM_END the CPU must
 *      execute the NEW opcode, so a stale A block would still write 0x11.
 *
 *   2. the magic literal loaded into r2
 *      A: ldr r2, =0xA1A1A1A1 -> SRAM[0x20000200] = 0xA1A1A1A1
 *      B: ldr r2, =0xB2B2B2B2 -> SRAM[0x20000200] = 0xB2B2B2B2
 *
 * No peripherals, no interrupts, no SysTick, no LCD: the firmware only proves
 * that the code the CPU executes (and the flash bytes it reads) changed.
 *
 * Instruction layout (validated at run time through the map magic):
 *   0x08000040 Reset_Handler: ldr r1, =0x20000200
 *   0x08000044 program_code:  movs r0, #0x11
 *   0x08000046               ldr r2, =0xA1A1A1A1
 *   0x0800004a               str r2, [r1]
 *   0x0800004c               str r0, [r1, #4]
 *   0x0800004e program_loop:  b program_loop
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
    ldr r1, =0x20000200

    .global program_code
    .thumb_func
program_code:
    movs r0, #0x11              /* <<< the differing OPCODE (B: #0x22) */

    ldr r2, =0xA1A1A1A1         /* <<< the differing magic  (B: 0xB2B2B2B2) */

    str r2, [r1]                /* SRAM[0x20000200] = magic                 */
    str r0, [r1, #4]            /* SRAM[0x20000204] = 0x11 (from the opcode) */

    .global program_loop
    .thumb_func
program_loop:
    b program_loop

    .thumb_func
Default_Handler:
    b Default_Handler

    .align 2
    .ltorg

/* ---- debug map: fixed address 0x08000200 (see linker.ld) -----------------
 * [0]  magic         0x50524F47 ("PROG")
 * [1]  Reset_Handler
 * [2]  program_code address   (the differing instruction)
 * [3]  magic address          0x20000200
 * [4]  opcode result address  0x20000204
 * [5]  magic value            A: 0xA1A1A1A1  B: 0xB2B2B2B2  (= build id)
 * [6]  opcode result value    A: 0x00000011  B: 0x00000022
 * [7]  initial SP (_estack)
 * [8]  program_loop address
 * [9..14] reserved
 * [15] end marker     0x50524F47
 */
    .section .debugmap, "a", %progbits
    .align 2
    .word 0x50524F47
    .word Reset_Handler
    .word program_code
    .word 0x20000200
    .word 0x20000204
    .word 0xA1A1A1A1
    .word 0x00000011
    .word _estack
    .word program_loop
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0x50524F47