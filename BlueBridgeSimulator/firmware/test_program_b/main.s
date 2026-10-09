/* CT117E-M4 virtual-flash programming test firmware -- build B (stage 7-2A).
 *
 * Byte-for-byte the same layout as build A (every instruction at the same
 * flash address); it differs only in the MOVS immediate at `program_code`
 * (0x22 instead of 0x11 -- a real OPCODE difference at the same virtual
 * address) and in the magic literal (0xB2B2B2B2 instead of 0xA1A1A1A1).
 * See firmware/test_program_a/main.s for the full explanation: this pair is
 * what proves there is no stale translated code after PROGRAM_END.
 *
 * Instruction layout (validated at run time through the map magic):
 *   0x08000040 Reset_Handler: ldr r1, =0x20000200
 *   0x08000044 program_code:  movs r0, #0x22
 *   0x08000046               ldr r2, =0xB2B2B2B2
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
    movs r0, #0x22              /* <<< the differing OPCODE (A: #0x11) */

    ldr r2, =0xB2B2B2B2         /* <<< the differing magic  (A: 0xA1A1A1A1) */

    str r2, [r1]                /* SRAM[0x20000200] = magic                 */
    str r0, [r1, #4]            /* SRAM[0x20000204] = 0x22 (from the opcode) */

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
    .word 0xB2B2B2B2
    .word 0x00000022
    .word _estack
    .word program_loop
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0x50524F47