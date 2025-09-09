OUTPUT_ARCH("riscv")
ENTRY(_entry)

SECTIONS {
  . = 0x80000000;

  .text : {
    *(.text .text.*)
    *(.rodata .rodata.*)
  }
  . = ALIGN(0x1000);
  PROVIDE(etext = .);

  .data : { *(.data .data.*) }
  . = ALIGN(0x1000);
  PROVIDE(edata = .);            /* 可选：有些代码用它做 bss 起点 */

  .bss (NOLOAD) : {
    __bss_start = .;             /* ★ 导出 bss 起点 */
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    __bss_end = .;               /* ★ 导出 bss 终点 */
  }
  . = ALIGN(0x1000);
  PROVIDE(end = .);

  .bss.stack (NOLOAD) : {
    *(.bss.stack)
  }
}
