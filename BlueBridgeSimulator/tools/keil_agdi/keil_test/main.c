/* B.3/B.4 Keil AGDI test fixture
 * (stage 7-2B.3 sections 55-58, stage 7-2B.4.2 sections 46-53,
 *  stage 7-2B.4.3 sections 6 / 23-58).
 *
 * This file exists ONLY as a source-level debug/download fixture: the Keil
 * project builds it (AC6, -O0, debug info on) so µVision can resolve
 * `main.c:<line>` / variable names to machine addresses through its own DWARF
 * reader.  The driver (BlueBridgeAGDI.dll) never parses AXF/ELF/DWARF/MAP.
 *
 * Layout (fixed enough for the test scripts):
 *   add_one(x)      - smallest leaf function, keeps x + a volatile local
 *   foo(x)          - calls add_one(); Step Over target (must not enter it)
 *   bar(x)          - calls foo(); Step Into target (must enter it)
 *   build_variant() - same-address opcode marker (A: 0x11, B: 0x22)
 *   main()          - seeds the B.4.2 markers, then loops in bar()
 *
 * Call chain for the source-debug acceptance (B.4.3):
 *   main -> bar -> foo -> add_one
 * Every level keeps its parameter and a volatile local alive at -O0 and writes
 * `g_step_marker` once before its call and once after it.  The marker pairs
 * make "did the step really enter / leave the callee" machine-checkable:
 *   0x11 inside add_one
 *   0x22 inside foo before the call      0x23 back inside foo after it
 *   0x33 inside bar before the call      0x34 back inside bar after it
 *
 * The B.4.2 A/B markers are switched by the test script with a plain text
 * substitution (backup + try/finally restore):
 *   BUILD_MAGIC_VALUE  0x11111111u (A) <-> 0x22222222u (B)
 *   BUILD_VARIANT_VALUE 0x11u      (A) <-> 0x22u      (B)
 * Both values keep identical code size, so build_variant() keeps the SAME
 * address across A and B -- the "same-address opcode" acceptance test.  The
 * download test's RAM-section scenario substitutes the text
 * `g_counter = build_magic;` in main() -- keep that statement spelled exactly
 * like that.
 */

#include <stdint.h>

#define BUILD_MAGIC_VALUE   0x11111111u   /* B.4.2 A/B: script-switched (A) */
#define BUILD_VARIANT_VALUE 0x11u         /* B.4.2 A/B: script-switched (A) */

/* Globals: B.4.2 A/B observables + B.4.3 Watch cross-check targets. */
volatile uint32_t build_magic = BUILD_MAGIC_VALUE;
volatile uint32_t g_counter;      /* main loop iteration count */
volatile uint32_t g_result;       /* bar() return value of the last iteration */
volatile uint32_t g_step_marker;  /* last function marker (see above) */
volatile uint32_t g_variant;      /* build_variant() result (B.4.2) */

/* Same-address opcode marker: only the immediate differs between A and B, so
 * the function address and size are identical in both builds (verified from the
 * Keil map file by the test script). */
__attribute__((noinline)) uint32_t build_variant(void)
{
    return BUILD_VARIANT_VALUE;
}

__attribute__((noinline)) int add_one(int x)
{
    volatile int local = x + 1;      /* Locals target: x and local */
    g_step_marker = 0x11;            /* marker: inside add_one */
    return local;
}

__attribute__((noinline)) int foo(int x)
{
    volatile int a = x + 10;
    int r;
    g_step_marker = 0x22;            /* marker: inside foo, before the call */
    r = add_one(a);                  /* Step Over target: must NOT enter add_one */
    g_step_marker = 0x23;            /* marker: back inside foo after the call */
    return r;
}

__attribute__((noinline)) int bar(int x)
{
    volatile int b = x + 20;
    int r;
    g_step_marker = 0x33;            /* marker: inside bar, before the call */
    r = foo(b);                      /* Step Into target: must enter foo */
    g_step_marker = 0x34;            /* marker: back inside bar after the call */
    return r;
}

int main(void)
{
    volatile int seed = 1;

    g_counter = build_magic;             /* B.4.2 ram-section substitution anchor */
    g_variant = build_variant();         /* forces the A/B opcode to execute */
    for (;;)
    {
        g_result = (uint32_t)bar((int)seed);   /* breakpoint target: main loop body */
        g_counter++;
        seed++;
    }
}