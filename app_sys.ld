ENTRY(_start)
SECTIONS
{
    /* シェル常駐帯域: 0x300000 (memmap.h: MEM_SHELL_LOAD_ADDR) */
    . = 0x300000;

    .text : {
        /* crt0.asm の _start を必ず先頭に配置 */
        *(.text.startup)
        *(.text*)
    }

    .data : {
        *(.data)
        *(.rodata*)
    }

    .bss : {
        __bss_start = .;
        *(.bss)
        *(COMMON)
        __bss_end = .;
    }
    _end = .;

    /DISCARD/ : {
        *(.note.gnu.build-id)
        *(.eh_frame)
        *(.comment)
    }
}
