ENTRY(kentry)
SECTIONS
{
    /* カーネルは 0x9000 番地にロードされる */
    . = 0x9000;

    .text : {
        /* kernel/kentry.o が確実に関数の先頭になるようにする */
        kernel/kentry.o(.text)
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

    /DISCARD/ : {
        *(.note.gnu.build-id)
        *(.eh_frame)
        *(.comment)
    }
}
