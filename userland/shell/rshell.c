#include "shell.h"
#include "config.h"
#include <stdlib.h>
#include "save/libos32save.h"   /* save_crc32 — CRC32 は既存実装を使う */

/* ======================================================================== */
/*  シリアル・リモート連携モジュール (rshell.c)                             */
/* ======================================================================== */

/* recv 用転送バッファ (BSS配置, 4KB — ストリーミングコピー用) */
static u8 xfer_buf[4096];

/* ------------------------------------------------------------------------ */
/*  ホットデプロイ用ステージングバッファ                                     */
/*                                                                          */
/*  ホストは NP21/W の aidebug API (POST /api/mem?space=linear) でこの配列へ */
/*  直接バイト列を書き込み、そのあと hotdeploy コマンドでファイル化させる。  */
/*  シリアルを通さないので hex 2 倍化もエミュレート速度も効かない。          */
/*  設計: docs/tasks/hotdeploy/DESIGN.md (案 A)                              */
/*                                                                          */
/*  シェル帯域は 0x300000 から 468KB (MEM_SHELL_MAX_SIZE)。text 約 58KB +    */
/*  既存 bss 約 59KB に 256KB を足しても収まる。                            */
/* ------------------------------------------------------------------------ */
#define HD_BUF_SIZE (256u * 1024u)
static u8 hd_buf[HD_BUF_SIZE];

static void cmd_serial(int argc, char **argv)
{
    int ret;
    (void)argc; (void)argv;
    g_api->serial_init(SYS_SERIAL_BAUD);
    g_api->kprintf(ATTR_GREEN, "RS-232C initialized (%ubps)\n", (u32)SYS_SERIAL_BAUD);

    /* serialfs 自動マウント (/host にマウント) */
    ret = g_api->sys_mount("/host", "COM1", "serialfs");
    if (ret == 0) {
        g_api->kprintf(ATTR_GREEN, "%s", "SerialFS mounted on /host\n");
    } else {
        g_api->kprintf(ATTR_YELLOW, "SerialFS mount skipped (%d)\n", ret);
    }
}

static void cmd_terminal(int argc, char **argv)
{
    int kch, sch;
    (void)argc; (void)argv;
    if (!g_api->serial_is_initialized()) {
        g_api->kprintf(ATTR_RED, "%s", "RS-232C not initialized. Run 'serial' first.\n");
        return;
    }
    g_api->kprintf(ATTR_CYAN, "%s", "Terminal mode (ESC to exit)\n");
    g_api->kprintf(ATTR_CYAN, "%s", "--------------------------------\n");
    for (;;) {
        kch = g_api->kbd_trygetchar();
        if (kch == 0x1B) break;
        if (kch >= 0) {
            g_api->serial_putchar((u8)kch);
            g_api->shell_putchar((char)kch, ATTR_GREEN);
        }
        sch = g_api->serial_trygetchar();
        if (sch >= 0) {
            if (sch == '\r') {
                g_api->shell_putchar('\n', ATTR_YELLOW);
            } else if (sch >= 0x20 || sch == '\n') {
                g_api->shell_putchar((char)sch, ATTR_YELLOW);
            }
        }
        if (kch < 0 && sch < 0) {
            /* 次の割り込み (PIT 100Hz / KBD / SER) まで CPU を止める。
             * get_tick の連打で 1 tick を潰していたが、リング 3 では
             * KAPI 1 回 6us でアイドルが CPU を振り切る。sys_halt なら
             * 待ち時間 (~1 tick) を保ったまま get_tick 呼び出しがゼロになる。 */
            u32 w = g_api->get_tick() + 1;
            while (g_api->get_tick() < w) g_api->sys_halt();
        }
    }
    g_api->kprintf(ATTR_CYAN, "%s", "\n[Terminal closed]\n");
}

static void cmd_rshell(int argc, char **argv)
{
    char rbuf[128];
    int rpos, ch, kch;
    (void)argc; (void)argv;

    if (!g_api->serial_is_initialized()) {
        g_api->kprintf(ATTR_RED, "%s", "Serial not initialized. Run 'serial' first.\n");
        return;
    }

    g_api->rshell_set_active(1);
    g_api->kprintf(ATTR_GREEN, "%s", "Remote shell active (ESC to exit)\n");
    g_api->kprintf(ATTR_CYAN, "%s", "Waiting for commands via serial...\n");
    g_api->serial_putchar(0x04);

    for (;;) {
        kch = g_api->kbd_trygetchar();
        if (kch == 0x1B) break;

        rpos = 0;
        rbuf[0] = '\0';

        if (kch >= 0x20 && kch < 0x7F) {
            ch = kch;
            goto read_rest;
        }

        for (;;) {
            ch = g_api->kbd_trygetchar();
            if (ch >= 0) {
                if (ch == 0x1B) goto rshell_exit;
                break;
            }
            {
                /* rshell コマンド待ち。sys_halt でアイドル時の get_tick
                 * 連打 (実測 311k/s) を止める。SER 受信 IRQ でも起きる。 */
                u32 w = g_api->get_tick() + 1;
                while (g_api->get_tick() < w) g_api->sys_halt();
            }
        }

    read_rest:
        while (ch >= 0 && ch != '\n' && ch != '\r' && rpos < 126) {
            rbuf[rpos++] = (char)ch;
            {
                int t = 0;
                while (t < 50000) {
                    ch = g_api->kbd_trygetchar();
                    if (ch >= 0) {
                        if (ch == 0x1B) goto rshell_exit;
                        break;
                    }
                    t++;
                }
            }
        }
        rbuf[rpos] = '\0';

        if (rpos == 0) continue;

        g_api->buz_off();

        if (rbuf[0]=='e' && rbuf[1]=='x' && rbuf[2]=='i' && rbuf[3]=='t' && rbuf[4]=='\0') break;

        g_api->kprintf(ATTR_YELLOW, "%s", "> ");
        g_api->kprintf(ATTR_WHITE, "%s", rbuf);
        g_api->kprintf(ATTR_WHITE, "%s", "\n");

        execute_command(rbuf);

        g_api->buz_off();

        {
            u32 wait_end = g_api->get_tick() + 1;
            while (g_api->get_tick() < wait_end) g_api->sys_halt();
        }

        g_api->serial_putchar(0x04);
    }
rshell_exit:
    g_api->rshell_set_active(0);
    g_api->kprintf(ATTR_CYAN, "%s", "\n[Remote shell closed]\n");
}

static void cmd_send(int argc, char **argv)
{
    int i;
    if (!g_api->serial_is_initialized()) {
        g_api->kprintf(ATTR_RED, "%s", "RS-232C not initialized. Run 'serial' first.\n");
        return;
    }
    for (i = 1; i < argc; i++) {
        g_api->serial_puts(argv[i]);
        if (i < argc - 1) g_api->serial_putchar(' ');
    }
    g_api->serial_putchar('\r');
    g_api->serial_putchar('\n');
    g_api->kprintf(ATTR_YELLOW, "%s", "Sent\n");
}

/* ------------------------------------------------------------------------ */
/*  hotdeploy — ステージングバッファの内容をファイルへ書き出す               */
/*                                                                          */
/*    hotdeploy                        ステージング領域の番地とサイズを報告  */
/*    hotdeploy PATH LEN CRC32         hd_buf[0..LEN) を PATH へ書き込む     */
/*                                                                          */
/*  出力はホスト側ラッパが読むので機械可読に保つこと                         */
/*  (HOTDEPLOY base= / HOTDEPLOY OK / HOTDEPLOY ERR)。                       */
/* ------------------------------------------------------------------------ */
static void cmd_hotdeploy(int argc, char **argv)
{
    const char *path;
    u32 len, want, got;
    int fd;

    /* 引数なし: ホストが書き込み先を知るための問い合わせ */
    if (argc < 2) {
        g_api->kprintf(ATTR_CYAN, "HOTDEPLOY base=0x%08X size=%u\n",
                       (u32)(unsigned long)hd_buf, (u32)HD_BUF_SIZE);
        return;
    }
    if (argc < 4) {
        g_api->kprintf(ATTR_RED, "%s", "HOTDEPLOY ERR usage\n");
        shell_print_help(argv[0]);
        return;
    }

    path = argv[1];
    len  = (u32)strtoul(argv[2], (char **)0, 0);
    want = (u32)strtoul(argv[3], (char **)0, 0);

    if (len == 0 || len > HD_BUF_SIZE) {
        g_api->kprintf(ATTR_RED, "HOTDEPLOY ERR bad-length %u\n", len);
        return;
    }

    /* ホストが書き終える前に叩かれた場合や、前回の残骸を掴んだ場合を
     * ここで落とす。CRC を必須にしているのはそのため。 */
    got = save_crc32(hd_buf, len);
    if (got != want) {
        g_api->kprintf(ATTR_RED, "HOTDEPLOY ERR crc want=0x%08X got=0x%08X\n",
                       want, got);
        return;
    }

    fd = g_api->sys_open(path, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC);
    if (fd < 0) {
        g_api->kprintf(ATTR_RED, "HOTDEPLOY ERR open %s\n", path);
        return;
    }
    if ((u32)g_api->sys_write(fd, hd_buf, len) != len) {
        g_api->kprintf(ATTR_RED, "%s", "HOTDEPLOY ERR write\n");
        g_api->sys_close(fd);
        return;
    }
    g_api->sys_close(fd);

    g_api->kprintf(ATTR_GREEN, "HOTDEPLOY OK %s %u\n", path, len);
}

/* host: プレフィックスを /host/ パスに変換するヘルパ */
static int resolve_host_path(const char *arg, char *out, int max)
{
    const char *p;
    int i;
    /* "host:" で始まるか判定 */
    if (arg[0] != 'h' || arg[1] != 'o' || arg[2] != 's' ||
        arg[3] != 't' || arg[4] != ':') return 0;

    p = arg + 5;  /* "host:" の後ろ */
    out[0] = '/'; out[1] = 'h'; out[2] = 'o'; out[3] = 's'; out[4] = 't';
    i = 5;
    if (*p != '/') { out[i++] = '/'; }  /* / を補完 */
    while (*p && i < max - 1) { out[i++] = *p++; }
    out[i] = '\0';
    return 1;
}

static void cmd_recv(int argc, char **argv)
{

    /* SerialFS モード: recv host:/path [localpath]
     *
     * パスは専用バッファに置く。以前は xfer_buf の先頭に置いたうえで
     * 同じ buf にデータを読み込んでおり、argc<3 のとき local_path が
     * xfer_buf の内部を指すという別名参照になっていた。 */
    static char host_path[256];
    if (argc >= 2 && resolve_host_path(argv[1], host_path, sizeof(host_path))) {
        const char *local_path;
        int fd_in, fd_out, n;
        u32 total;
        u32 t0, t1, elapsed;

        /* ローカルパスの決定 */
        if (argc >= 3) {
            local_path = argv[2];
        } else {
            /* ファイル名のみ抽出 */
            const char *p = host_path;
            const char *last_slash = host_path;
            while (*p) { if (*p == '/') last_slash = p + 1; p++; }
            local_path = last_slash;
        }

        g_api->kprintf(ATTR_CYAN, "Downloading: %s -> %s\n", host_path, local_path);
        t0 = g_api->get_tick();

        fd_in = g_api->sys_open(host_path, KAPI_O_RDONLY);
        if (fd_in < 0) {
            g_api->kprintf(ATTR_RED, "recv: %s not found\n", host_path);
            return;
        }
        fd_out = g_api->sys_open(local_path, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC);
        if (fd_out < 0) {
            g_api->kprintf(ATTR_RED, "recv: cannot create %s\n", local_path);
            g_api->sys_close(fd_in);
            return;
        }

        /* xfer_buf 単位で読み切るまで回す。以前は sys_read が 1 回だけで、
         * 4096 バイトを超えるファイルは黙って切り詰められていた。 */
        total = 0;
        for (;;) {
            n = g_api->sys_read(fd_in, xfer_buf, sizeof(xfer_buf));
            if (n < 0) {
                g_api->kprintf(ATTR_RED, "%s", "recv: read failed\n");
                g_api->sys_close(fd_in);
                g_api->sys_close(fd_out);
                return;
            }
            if (n == 0) break;
            if ((int)g_api->sys_write(fd_out, xfer_buf, (u32)n) != n) {
                g_api->kprintf(ATTR_RED, "%s", "recv: write failed\n");
                g_api->sys_close(fd_in);
                g_api->sys_close(fd_out);
                return;
            }
            total += (u32)n;
        }
        g_api->sys_close(fd_in);
        g_api->sys_close(fd_out);

        t1 = g_api->get_tick();
        elapsed = t1 - t0;
        if (elapsed == 0) elapsed = 1;
        g_api->kprintf(ATTR_GREEN, "Received %u bytes", total);
        g_api->kprintf(ATTR_GREEN, " (%u.%02us, ",
                       elapsed / 100, elapsed % 100);
        g_api->kprintf(ATTR_GREEN, "%u B/s)\n",
                       total * 100 / elapsed);
        return;
    }

    /* 引数なし or host: プレフィックスなし */
    shell_print_help(argv[0]);
}

static void cmd_push(int argc, char **argv)
{
    char host_path[256];
    const char *local_path;
    int fd_in, fd_out, sz;
    u32 t0, t1, elapsed;

    if (argc < 3) {
        shell_print_help(argv[0]);
        return;
    }

    local_path = argv[1];

    /* 宛先が host: プレフィックスか確認 */
    if (!resolve_host_path(argv[2], host_path, 256)) {
        g_api->kprintf(ATTR_RED, "%s", "push: destination must be host:path\n");
        return;
    }

    g_api->kprintf(ATTR_CYAN, "Uploading: %s -> %s\n", local_path, host_path);
    t0 = g_api->get_tick();

    fd_in = g_api->sys_open(local_path, KAPI_O_RDONLY);
    if (fd_in < 0) {
        g_api->kprintf(ATTR_RED, "push: %s not found\n", local_path);
        return;
    }
    sz = g_api->sys_read(fd_in, xfer_buf, sizeof(xfer_buf));
    g_api->sys_close(fd_in);
    if (sz < 0) {
        g_api->kprintf(ATTR_RED, "%s", "push: read failed\n");
        return;
    }

    fd_out = g_api->sys_open(host_path, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC);
    if (fd_out < 0) {
        g_api->kprintf(ATTR_RED, "push: cannot create %s\n", host_path);
        return;
    }
    if ((int)g_api->sys_write(fd_out, xfer_buf, (u32)sz) != sz) {
        g_api->kprintf(ATTR_RED, "%s", "push: write failed\n");
        g_api->sys_close(fd_out);
        return;
    }
    g_api->sys_close(fd_out);

    t1 = g_api->get_tick();
    elapsed = t1 - t0;
    if (elapsed == 0) elapsed = 1;
    g_api->kprintf(ATTR_GREEN, "Sent %d bytes", sz);
    g_api->kprintf(ATTR_GREEN, " (%u.%02us, ",
                   elapsed / 100, elapsed % 100);
    g_api->kprintf(ATTR_GREEN, "%u B/s)\n",
                   (u32)sz * 100 / elapsed);
}

static void cmd_tvdump(int argc, char **argv)
{
    volatile u16 *text = (volatile u16 *)0xA0000UL;
    volatile u8  *attr_base = (volatile u8 *)0xA2000UL;
    int row, col;
    (void)argc; (void)argv;

    if (!g_api->serial_is_initialized()) {
        g_api->kprintf(ATTR_RED, "%s", "Serial not initialized.\n");
        return;
    }

    g_api->serial_putchar('T'); g_api->serial_putchar('V');
    g_api->serial_putchar('D'); g_api->serial_putchar('M');
    g_api->serial_putchar(80);  g_api->serial_putchar(25);

    for (row = 0; row < 25; row++) {
        for (col = 0; col < 80; col++) {
            int idx = row * 80 + col;
            u16 ch_val = text[idx];
            u8  at = attr_base[idx * 2];
            g_api->serial_putchar((u8)(ch_val & 0xFF));
            g_api->serial_putchar(at);
        }
    }
    g_api->kprintf(ATTR_GREEN, "%s", "TVRAM dump sent\n");
}

/* 登録用テーブル */
static const ShellCmd rshell_cmds[] = {
    { "serial",   cmd_serial,   "",              "Init RS-232C + mount SerialFS" },
    { "terminal", cmd_terminal, "",              "Enter serial terminal mode" },
    { "rshell",   cmd_rshell,   "",              "Start remote shell host" },
    { "send",     cmd_send,     "TEXT...",       "Send text via serial" },
    { "hotdeploy", cmd_hotdeploy, "[PATH LEN CRC32]",
      "Write staging buffer to file (no args: report buffer address)" },
    { "recv",     cmd_recv,     "[host:PATH [LOCAL]]", "Receive file (SerialFS or legacy)" },
    { "push",     cmd_push,     "LOCAL host:PATH",     "Upload file to host via SerialFS" },
    { "tvdump",   cmd_tvdump,   "",              "Dump Text VRAM over serial" },
    { (const char *)0, 0, 0, 0 }
};

void shell_rshell_init(void)
{
    shell_register_cmds(rshell_cmds);
}
