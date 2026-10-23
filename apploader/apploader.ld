/* Loaders copy the apploader body to 0x81200000 and call its entry point there */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(apploader_entry)

SECTIONS
{
  /* The apploader.img header, just ahead of the body so the flat binary starts with it, then the date, the entry point,
     the body's size and the trailer's size */
  . = 0x81200000 - 0x20;
  .header : {
    LONG(0x32303236) LONG(0x2f31302f) LONG(0x30390000) LONG(0)
    LONG(apploader_entry)
    LONG(apploader_end - apploader_start)
    LONG(0)
    LONG(0)
  }

  . = 0x81200000;
  apploader_start = .;

  .text : {
    *(.text.entry)
    *(.text .text.*)
  }

  .rodata : {
    *(.rodata .rodata.*)
  }

  /* bss is kept inside .data so the flat binary carries it as zeros */
  .data : ALIGN(32) {
    *(.data .data.*)
    *(.sdata .sdata.*)
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(32);
  }

  apploader_end = .;

  /DISCARD/ : {
    *(.comment)
    *(.eh_frame)
  }
}
