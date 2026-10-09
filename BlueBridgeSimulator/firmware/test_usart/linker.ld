/* STM32G431RBT6 test firmware linker script.
 * Flash 128K @ 0x08000000, SRAM 22K @ 0x20000000.
 * The .selfdata mailbox is pinned at the very start of SRAM (0x20000000)
 * so the simulator self test can peek it at a fixed address. */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 22K
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
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  /* self-test mailbox at 0x20000000, initialized from flash */
  _siselfdata = LOADADDR(.selfdata);
  .selfdata 0x20000000 :
  {
    _sselfdata = .;
    KEEP(*(.selfdata))
    . = ALIGN(4);
    _eselfdata = .;
  } > RAM AT > FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  .bss (NOLOAD) :
  {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /DISCARD/ :
  {
    *(.ARM.exidx*)
    *(.ARM.attributes)
  }
}