/* ======================================================================== */
/*  OS32_SQLITE_TEST.C — カーネル内 SQLite 動作検証                          */
/* ======================================================================== */

#include "os32_sqlite_config.h"
#include "sqlite3.h"
#include "types.h"

/* テキストVRAM直接書き込み (kprintf 使用不可 — コンソール未初期化の可能性) */
#define TVRAM_CHAR ((volatile u16 *)0xA0000UL)
#define TVRAM_ATTR ((volatile u16 *)0xA2000UL)

static void sqt_mark(int row, int col, const char *str, u8 attr)
{
    int pos = row * 80 + col;
    while (*str) {
        TVRAM_CHAR[pos] = (u16)(u8)*str;
        TVRAM_ATTR[pos] = (u16)attr;
        str++;
        pos++;
    }
}

static void sqt_hex(int row, int col, u32 val, u8 attr)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;
    for (i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
    sqt_mark(row, col, buf, attr);
}

/* カーネル内 SQLite テスト — 段階的デバッグ版 (TVRAM直書き, 6行目使用) */
int os32_sqlite_test(void)
{
    void *p;
    sqlite3 *db;
    int rc;
    int row = 6;  /* 起動ログの下に表示 */

    sqt_mark(row, 0, "SQT:", 0x05);

    /* 1. sqlite3_malloc テスト */
    sqt_mark(row, 4, "M", 0x06);
    p = sqlite3_malloc(256);
    if (!p) { sqt_mark(row, 5, "X", 0x02); return -1; }
    sqt_mark(row, 5, "O", 0x0A);
    sqt_hex(row, 6, (u32)p, 0x06);

    sqt_mark(row, 15, "F", 0x06);
    sqlite3_free(p);
    sqt_mark(row, 16, "O", 0x0A);

    /* 2. sqlite3_open */
    sqt_mark(row, 18, "P", 0x06);
    db = 0;
    rc = sqlite3_open(":memory:", &db);
    /* rc を文字として直書き */
    {
        char buf[2];
        buf[0] = (char)('0' + rc);
        buf[1] = '\0';
        sqt_mark(row, 19, buf, (rc == 0) ? 0x0A : 0x02);
    }

    if (rc == SQLITE_OK) {
        /* 3. sqlite3_exec */
        sqt_mark(row, 21, "E", 0x06);
        rc = sqlite3_exec(db, "SELECT 1", 0, 0, 0);
        {
            char buf[2];
            buf[0] = (char)('0' + rc);
            buf[1] = '\0';
            sqt_mark(row, 22, buf, (rc == 0) ? 0x0A : 0x02);
        }

        /* 4. close */
        sqt_mark(row, 24, "C", 0x06);
        sqlite3_close(db);
        sqt_mark(row, 25, "O", 0x0A);
    }

    sqt_mark(row, 27, "END", 0x0A);
    return 0;
}


