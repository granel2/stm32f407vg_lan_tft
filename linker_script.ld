/* STM32F407VG Flash and RAM layout */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 128K
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
    KEEP(*(.text*))
    KEEP(*(.rodata*))
    KEEP(*(.glue_7))
    KEEP(*(.glue_7t))
    KEEP(*(.gcc_except_table))
  } > FLASH

  .rodata :
  {
    KEEP(*(.rodata*))
  } > FLASH

  .ARM.extab :
  {
    KEEP(*(.ARM.extab*));
  } > FLASH

  .ARM :
  {
    KEEP(*(.ARM.*))
  } > FLASH

  .data :
  {
    __data_start__ = .;
    _sdata = .;
    KEEP(*(.data*))
    KEEP(*(.data))
    __data_end__ = .;
    _edata = .;
  } > RAM AT> FLASH
  /* Where Reset_Handler copies .data's initial values *from* in FLASH -
     must be computed via LOADADDR(), not just "."; see startup_stm32f407xx.s */
  _sidata = LOADADDR(.data);

  .bss :
  {
    __bss_start__ = .;
    _sbss = .;
    KEEP(*(.bss*))
    KEEP(*(.bss))
    __bss_end__ = .;
    _ebss = .;
  } > RAM

  /* Heap end marker for dynamic memory allocation */
  end = .;

  .stack (COPY):
  {
    . = ALIGN(8);
    __stack_start__ = .;
    . = . + 0x400;
    __stack_end__ = .;
  } > RAM

  /DISCARD/ :
  {
    *(.note.GNU-stack)
  }
}
