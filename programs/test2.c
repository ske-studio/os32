#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* ======================================================================== */
/*  TEST2.C — KernelAPI v2 テストプログラム                                  */
/*                                                                          */
/*  新APIの動作確認:                                                         */
/*    - file_write / file_read (ファイルI/O)                                */
/*    - mem_alloc / mem_free   (メモリ管理)                                  */
/*    - get_tick               (タイマ)                                      */
/*    - print_dec              (数値出力)                                    */
/*                                                                          */
/*  ビルド:                                                                  */
/*    wcc386 -s -zl -zls -mf -3s test2.c -fo=test2.obj                      */
/*    wlink FORMAT RAW BIN NAME test2.bin FILE test2.obj                     */
/*          OPTION NODEFAULTLIBS, START=main, OFFSET=0x400000                */
/* ======================================================================== */





/* KernelAPI v2 構造体 (exec.h と同じレイアウト)
 * 注意: 関数ポインタは __cdecl で宣言すること。
 * カーネル側のラッパーが cdecl で定義されているため。 */
#include "os32api.h"

/* ヘルパー関数の前方宣言 (mainをバイナリ先頭に配置するため) */
static int my_memcmp(const void *a, const void *b, u32 n);

#define GREEN  0xA2
#define RED    0x42
#define CYAN   0x62
#define WHITE  0xE1

/* main はバイナリの先頭に配置される必要がある */
void main(int argc, char **argv, KernelAPI *api)
{
    int ok = 1;

    api->kprintf(CYAN, "%s", "=== KernelAPI v2 Test ===\n");

    /* --- バージョンチェック --- */
    api->kprintf(WHITE, "%s", "  version: ");
    api->kprintf(WHITE, "%d", api->version);
    api->kprintf(WHITE, "%s", "\n");
    if (api->version < 2) {
        api->kprintf(RED, "%s", "  ERROR: v2 required!\n");
        return;
    }

    /* --- タイマテスト --- */
    {
        u32 t1, t2;
        api->kprintf(WHITE, "%s", "  get_tick: ");
        t1 = api->get_tick();
        api->kprintf(GREEN, "%d", t1);
        /* 少し時間消費 */
        { volatile u32 i; for (i = 0; i < 100000; i++); }
        t2 = api->get_tick();
        api->kprintf(WHITE, "%s", " -> ");
        api->kprintf(GREEN, "%d", t2);
        if (t2 >= t1) {
            api->kprintf(GREEN, "%s", " OK\n");
        } else {
            api->kprintf(RED, "%s", " FAIL\n");
            ok = 0;
        }
    }

    /* --- メモリテスト --- */
    {
        char *buf = (char *)api->mem_alloc(256);
        api->kprintf(WHITE, "%s", "  mem_alloc(256): ");
        if (buf) {
            api->kprintf(GREEN, "%s", "OK @");
            api->kprintf(GREEN, "%d", (u32)buf);
            api->kprintf(WHITE, "%s", "\n");
            /* 書き込みテスト */
            buf[0] = 'H'; buf[1] = 'i'; buf[2] = 0;
            api->kprintf(WHITE, "%s", "  mem write: ");
            api->kprintf(GREEN, "%s", buf);
            api->kprintf(WHITE, "%s", "\n");
            api->mem_free(buf);
            api->kprintf(GREEN, "%s", "  mem_free: OK\n");
        } else {
            api->kprintf(RED, "%s", "FAIL\n");
            ok = 0;
        }
    }

    /* --- ファイルI/Oテスト --- */
    {
        const char *test_data = "API-TEST-OK";
        char read_buf[64];
        int wr, rd;

        api->kprintf(WHITE, "%s", "  file_write: ");
        int t_fd=api->sys_open("/api_test.txt", 1|0x100|0x200); if(t_fd>=0){ wr=api->sys_write(t_fd, test_data, 11); api->sys_close(t_fd); } else wr=-1;
        if (wr == 0) {
            api->kprintf(GREEN, "%s", "OK\n");
        } else {
            api->kprintf(RED, "%s", "FAIL rc=");
            api->kprintf(RED, "%d", (u32)(wr < 0 ? -wr : wr));
            api->kprintf(RED, "%s", "\n");
            ok = 0;
        }

        api->kprintf(WHITE, "%s", "  file_read:  ");
        int r_fd=api->sys_open("/api_test.txt", 0); if(r_fd>=0){ rd=api->sys_read(r_fd, read_buf, 63); api->sys_close(r_fd); } else rd=-1;
        if (rd == 11 && my_memcmp(read_buf, test_data, 11) == 0) {
            read_buf[rd] = 0;
            api->kprintf(GREEN, "%s", read_buf);
            api->kprintf(GREEN, "%s", " OK\n");
        } else {
            api->kprintf(RED, "%s", "FAIL rd=");
            api->kprintf(RED, "%d", (u32)(rd < 0 ? -rd : rd));
            api->kprintf(RED, "%s", "\n");
            ok = 0;
        }
    }

    /* --- 結果 --- */
    if (ok) {
        api->kprintf(GREEN, "%s", "=== ALL TESTS PASSED ===\n");
    } else {
        api->kprintf(RED, "%s", "=== SOME TESTS FAILED ===\n");
    }
}

/* ======================================================================== */
/*  ヘルパー関数（mainの後に配置）                                          */
/* ======================================================================== */

static int my_memcmp(const void *a, const void *b, u32 n)
{
    const u8 *p = (const u8 *)a;
    const u8 *q = (const u8 *)b;
    u32 i;
    for (i = 0; i < n; i++) {
        if (p[i] != q[i]) return 1;
    }
    return 0;
}
