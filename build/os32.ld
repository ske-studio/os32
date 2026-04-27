ENTRY(kentry)

SECTIONS
{
    /* カーネルは 0x9000 番地にロードされる */
    . = 0x9000;

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
    /*  SQLite 拡張域 (0x18A000 - 0x23FFFF, 728KB, -O0用)            */
    /* ================================================================ */
    . = 0x18A000;

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

    /DISCARD/ : {
        *(.note.gnu.build-id)
        *(.eh_frame)
        *(.comment)
    }
}
