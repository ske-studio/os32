ENTRY(_start)
SECTIONS
{
    /* 外部プログラムのロードアドレス (= include/memmap.h の MEM_EXEC_LOAD_ADDR)。
     * 0x400000-0x4FFFFF は共有ライブラリ帯域 (MEM_SHLIB_BASE) になったので
     * 2026-09-05 (GUI v1.1 K3) に 1MB 上へ移動した。ここを変えたら
     * memmap.h と mkos32x.py の load_addr 判定も必ず合わせること。 */
    . = 0x500000;

    .text : {
        /* crt0.asm の _start を必ず先頭に配置 */
        *(.text.startup)
        *(.text*)
    }

    .data : {
        *(.data)
        *(.data.*)
        *(.rodata*)
    }

    .bss : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
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
