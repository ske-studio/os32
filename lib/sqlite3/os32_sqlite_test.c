/* ======================================================================== */
/*  OS32_SQLITE_TEST.C — カーネル内 SQLite 動作検証                          */
/* ======================================================================== */

#include "os32_sqlite_config.h"
#include "sqlite3.h"
#include "types.h"

/* tvram 関数 (INC_SQLITE に tvram.h がないため extern 宣言) */
extern void tvram_putchar_at(int x, int y, char ch, u8 attr);

static void mark(int col, char ch, u8 attr)
{
    tvram_putchar_at(col, 24, ch, attr);
}

/* カーネル内 SQLite テスト — 段階的 */
int os32_sqlite_test(void)
{
    void *p;
    sqlite3 *db;
    int rc;

    /* 1. sqlite3_malloc テスト */
    mark(0, '1', 0x05);
    p = sqlite3_malloc(256);
    if (!p) { mark(1, 'X', 0x02); return -1; }
    mark(1, 'O', 0x0A);
    sqlite3_free(p);
    mark(2, 'F', 0x0A);

    /* 2. sqlite3_memory_used */
    mark(3, '2', 0x05);
    {
        int used = (int)sqlite3_memory_used();
        mark(4, (used == 0) ? '0' : '+', 0x06);
    }

    /* 3. sqlite3_open */
    mark(5, '3', 0x05);
    db = 0;
    rc = sqlite3_open(":memory:", &db);
    mark(6, '0' + rc, (rc == 0) ? 0x0A : 0x02);

    if (rc == SQLITE_OK) {
        /* 4. simple exec */
        mark(7, '4', 0x05);
        rc = sqlite3_exec(db, "SELECT 1", 0, 0, 0);
        mark(8, '0' + rc, (rc == 0) ? 0x0A : 0x02);

        /* 5. close */
        mark(9, '5', 0x05);
        sqlite3_close(db);
        mark(10, 'D', 0x0A);
    }

    /* 完了 */
    mark(12, 'E', 0x0A);
    mark(13, 'N', 0x0A);
    mark(14, 'D', 0x0A);

    return 0;
}
