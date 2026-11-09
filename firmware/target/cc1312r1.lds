/* CC1312R1F3: 352 KB flash (last 88 bytes = CCFG), 80 KB SRAM. Symbols match
 * the CoreSDK NoRTOS startup file. */

MIN_STACKSIZE = 0x800;

MEMORY
{
    FLASH (RX)      : ORIGIN = 0x00000000, LENGTH = 0x00057FA8
    FLASH_CCFG (RX) : ORIGIN = 0x00057FA8, LENGTH = 0x00000058
    SRAM (RWX)      : ORIGIN = 0x20000000, LENGTH = 0x00014000
}

SECTIONS
{
    .resetVecs 0x0 : { KEEP(*(.resetVecs)) } > FLASH

    /* The NoRTOS HwiP installs handlers in this RAM table (VTOR). */
    .ramVecs (NOLOAD) : ALIGN(1024) { KEEP(*(.ramVecs)) } > SRAM

    .text : {
        *(.text) *(.text.*)
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(.init_array*))
        __init_array_end = .;
        *(.init) *(.fini*)
    } > FLASH

    .rodata : { *(.rodata) *(.rodata.*) } > FLASH

    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > FLASH

    .data : ALIGN(4) {
        __data_load__ = LOADADDR(.data);
        __data_start__ = .;
        *(.data) *(.data.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > SRAM AT> FLASH

    .ccfg : { KEEP(*(.ccfg)) } > FLASH_CCFG

    .bss (NOLOAD) : {
        __bss_start__ = .;
        *(.bss) *(.bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > SRAM

    /* newlib-nano's sbrk; the firmware does not allocate, but libc links it. */
    .heap (NOLOAD) : ALIGN(8) {
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . += 0x100;
    } > SRAM

    _stack_end = ORIGIN(SRAM) + LENGTH(SRAM);
    ASSERT(_stack_end - . >= MIN_STACKSIZE, "no room left for the stack")
}
