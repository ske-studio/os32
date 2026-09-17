ENTRY(kentry)

/* ======================================================================== */
/*  include/memmap.h からの写し (2026-09-17 決裁 D2)                         */
/*                                                                          */
/*  リンカスクリプトは C のヘッダを読めないので、ここだけ値を二重に持つ。    */
/*  **一致は `python3 tools/gen_memmap.py --check` が機械的に照合する**      */
/*  (make check の中)。片方だけ直すと赤になる ([C4])。                       */
/*                                                                          */
/*  MEM_KSTACK_TOP  kernel/kentry.asm が `extern` で参照する ESP の初期値。  */
/*                  ASM からは C のマクロを引けないので絶対シンボルで渡す。  */
/*  MEM_KERNEL_IMAGE_MAX  カーネル本体 (.text+.data+.bss) に使える最大。     */
/*                  カーネル帯域 1MB から、ヒープ 320KB・KAPI 4KB・          */
/*                  ガード 2 枚・SHM 224KB を引いた残り。                    */
/* ======================================================================== */
KERNEL_LOAD_ADDR     = 0x100000;
MEM_KSTACK_TOP       = 0x2FFFFC;
MEM_KERNEL_IMAGE_MAX = 0x75000;

SECTIONS
{
    /* カーネルは 0x100000 (1MB) にロードされる */
    . = 0x100000;

    .text : {
        /* kernel/kentry.o が確実に関数の先頭になるようにする */
        kernel/kentry.o(.text)
        /* SQLite関連ファイルを除外して残りを配置 */
        *(EXCLUDE_FILE(lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o) .text)
        *(EXCLUDE_FILE(lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o) .text.*)
    }

    .data : {
        *(EXCLUDE_FILE(lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o) .data)
        *(EXCLUDE_FILE(lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o) .data.*)
        *(EXCLUDE_FILE(lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o) .rodata)
        *(EXCLUDE_FILE(lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o) .rodata.*)
    }

    .bss : {
        __bss_start = .;
        *(EXCLUDE_FILE(lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o) .bss)
        *(EXCLUDE_FILE(lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o) .bss.*)
        *(EXCLUDE_FILE(lib/sqlite3/sqlite3.o lib/sqlite3/os32_sqlite_vfs.o lib/sqlite3/os32_sqlite_test.o) COMMON)
        __bss_end = .;
    }

    /* ================================================================ */
    /*  SQLite 拡張域 (0x200000 - 0x2FFFFF, 1MB)                       */
    /* ================================================================ */
    . = 0x200000;

    .sqlite_text : {
        __sqlite_start = .;
        KEEP(lib/sqlite3/sqlite3.o(.text .text.*))
        KEEP(lib/sqlite3/os32_sqlite_vfs.o(.text .text.*))
        KEEP(lib/sqlite3/os32_sqlite_test.o(.text .text.*))
    }

    .sqlite_rodata : {
        KEEP(lib/sqlite3/sqlite3.o(.rodata .rodata.*))
        KEEP(lib/sqlite3/os32_sqlite_vfs.o(.rodata .rodata.*))
        KEEP(lib/sqlite3/os32_sqlite_test.o(.rodata .rodata.*))
    }

    .sqlite_data : {
        KEEP(lib/sqlite3/sqlite3.o(.data .data.*))
        KEEP(lib/sqlite3/os32_sqlite_vfs.o(.data .data.*))
        KEEP(lib/sqlite3/os32_sqlite_test.o(.data .data.*))
        __sqlite_data_end = .;
    }

    .sqlite_bss (NOLOAD) : {
        lib/sqlite3/sqlite3.o(.bss .bss.* COMMON)
        lib/sqlite3/os32_sqlite_vfs.o(.bss .bss.* COMMON)
        lib/sqlite3/os32_sqlite_test.o(.bss .bss.* COMMON)
        __sqlite_end = .;
    }

    /* ==================================================================== */
    /*  カーネル本体の予算 (2026-09-17 決裁 D2、票                            */
    /*  docs/tasks/memory/TASK_KSTACK_USER.md)                               */
    /*                                                                       */
    /*  KHEAP_BASE は __bss_end を 4KB に切り上げた位置で、そこから           */
    /*  ヒープ → KAPI → SHM が芋づるで動く。カーネルが予算を超えると SHM の   */
    /*  後方ガードがカーネル帯域を突き抜け、**静かに**他人のページを          */
    /*  not-present にする。2026-09-17 にはカーネルスタックの真ん中に         */
    /*  居座り、あと 224 バイトで三重フォルトするところまで来ていた。         */
    /*  それを **ここで、リンク時に** 止める。                                */
    /* ==================================================================== */
    ASSERT(__bss_end <= KERNEL_LOAD_ADDR + MEM_KERNEL_IMAGE_MAX,
        "kernel image exceeds MEM_KERNEL_IMAGE_MAX (include/memmap.h): the floating KHEAP_BASE/KAPI/SHM chain would run past the kernel band and silently unmap someone else's pages. Fix: shrink the kernel, or reduce MEM_SHM_SIZE in 16KB steps (keep SHM_BLOCK_COUNT in kernel/shm.h in sync), or reduce KHEAP_SIZE. Run 'python3 tools/gen_memmap.py --check' for the exact numbers; background is docs/tasks/memory/TASK_KSTACK_USER.md")

    /DISCARD/ : {
        *(.note.gnu.build-id)
        *(.eh_frame)
        *(.comment)
    }
}
