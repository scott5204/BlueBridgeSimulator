/* STM32G431RBT6 debug-target test firmware linker script (stage 7-1).
 * Flash 128K @ 0x08000000, SRAM 32K @ 0x20000000 (as modelled by the SoC).
 *
 * The .debugmap section is pinned at 0x08000200 so the self test can read the
 * label addresses before the firmware has run. If the code ever grows into
 * that address the LINK FAILS (an intentional static check). */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } > FLASH

  .text :
  {
    *(.text*)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  /* debug map at a fixed address -- must not overlap .text */
  .debugmap 0x08000200 :
  {
    KEEP(*(.debugmap))
    . = ALIGN(4);
  } > FLASH

  /DISCARD/ :
  {
    *(.ARM.exidx*)
    *(.ARM.attributes)
  }
}