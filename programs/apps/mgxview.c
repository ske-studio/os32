/* ======================================================================== */
/*  MGXVIEW.C — OS32 漫画専用モノクロ画像 (MGX) ビューワ                    */
/*                                                                          */
/*  MGX は 1bpp モノクロ専用形式。エンコードはホスト側 (tools/img2mgx.py)    */
/*  のみで、ゲスト側は本ビューワによる表示のみを提供する。                   */
/*  展開は libos32mgx (中身は zlib 付属の参照 inflate lib/puff.c) が行う。   */
/*                                                                          */
/*  カレントディレクトリの *.MGX を名前順に並べて 1 冊として扱い、           */
/*  メモリが許す限り展開済みページをキャッシュに先読みする。                 */
/*  キャッシュに載っているページへの移動はファイル I/O も展開も発生しない。  */
/*                                                                          */
/*  bpp=1..4 可変。格納されているプレーン数だけ gfx_fb.planes[] へ転送し、   */
/*  各画素値をどのグレー階調で表示するかはヘッダのパレット表が決める。      */
/*  bpp が小さいほど展開量も転送量も減るので、そのぶん速い。                */
/*                                                                          */
/*  使い方: exec mgxview [FILE.MGX] [-k] [-i]                              */
/*    引数なし : カレントディレクトリの先頭ページから表示                    */
/*    FILE.MGX : そのファイルのあるディレクトリを開き、そのページから表示    */
/*    -k : 終了時にVRAMをクリアしない                                        */
/*    -i : 白黒反転                                                          */
/*  キー: S / → = 次ページ, A / ← = 前ページ                                 */
/*        F / TAB = ファイラ (別のページ/ディレクトリを選ぶ)                 */
/*        ESC / q = 終了                                                     */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "os32api.h"
#include "libos32gfx.h"
#include "libos32mgx.h"
#include "libos32filer.h"

/* ---- キーボードコード (vdpview.c と同じ) ---- */
#define KEY_ESC     0x1B
#define KEY_SPACE   ' '
#define KEY_RIGHT   0x1C
#define KEY_LEFT    0x1D
#define KEY_TAB     0x09

/* ---- 画面 ---- */
#define SCREEN_W    640
#define SCREEN_H    400

/* ---- ページ配列 ---- */
#define MAX_PAGES     256
#define NAME_MAX_LEN   48
#define PATH_MAX_LEN  160

/* 先読みでキャッシュを埋める最大枚数。mem_alloc が尽きた時点でも止まる。 */
#define PREFETCH_RADIUS  MAX_PAGES

typedef struct {
    char name[NAME_MAX_LEN];
    u8  *bitmap;      /* 展開済みプレーンデータ。NULL = 未キャッシュ */
    u32  raw_size;
    u32  plane_size;
    MgxHeader hdr;    /* パレット表を含むヘッダ (描画時に参照する) */
    u8   failed;      /* 読み込みに失敗した (再試行しない) */
} Page;

static KernelAPI *api;

static Page  pages[MAX_PAGES];
static int   npages;
static char  dirpath[PATH_MAX_LEN];
static int   cache_full;      /* mem_alloc が尽きた */
static int   cached_count;
static u32   cached_bytes;

/* ------------------------------------------------------------------------
 *  エラー表示
 * ------------------------------------------------------------------------ */
static const char *mgx_errstr(int e)
{
    switch (e) {
    case MGX_ERR_MAGIC:   return "not an MGX file";
    case MGX_ERR_VERSION: return "unsupported version";
    case MGX_ERR_HEADER:  return "bad header";
    case MGX_ERR_TRUNC:   return "truncated file";
    case MGX_ERR_CODEC:   return "unknown codec";
    case MGX_ERR_STREAM:  return "corrupt compressed stream";
    case MGX_ERR_SIZE:    return "size mismatch";
    case MGX_ERR_CKSUM:   return "checksum mismatch";
    case MGX_ERR_ARG:     return "bad argument";
    default:              return "unknown error";
    }
}

/* ------------------------------------------------------------------------
 *  文字列ユーティリティ
 * ------------------------------------------------------------------------ */
static char lower_ch(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static int name_cmp_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = lower_ch(*a);
        char cb = lower_ch(*b);
        if (ca != cb)
            return (int)((u8)ca) - (int)((u8)cb);
        a++; b++;
    }
    return (int)((u8)lower_ch(*a)) - (int)((u8)lower_ch(*b));
}

/* 末尾が ".mgx" (大文字小文字問わず) か */
static int has_mgx_ext(const char *name)
{
    int n = (int)strlen(name);
    if (n < 5)
        return 0;
    return lower_ch(name[n - 4]) == '.'
        && lower_ch(name[n - 3]) == 'm'
        && lower_ch(name[n - 2]) == 'g'
        && lower_ch(name[n - 1]) == 'x';
}

static void copy_str(char *dst, int cap, const char *src)
{
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* dirpath と name を連結 */
static void build_path(char *out, int cap, const char *name)
{
    int i = 0, j = 0;

    while (dirpath[j] && i < cap - 2)
        out[i++] = dirpath[j++];
    if (i > 0 && out[i - 1] != '/' && out[i - 1] != '\\')
        out[i++] = '/';
    j = 0;
    while (name[j] && i < cap - 1)
        out[i++] = name[j++];
    out[i] = '\0';
}

/* ------------------------------------------------------------------------
 *  ディレクトリ走査
 * ------------------------------------------------------------------------ */
static void sort_pages(void);
static void restore_default_palette(void);
static void split_path(const char *path, char *namebuf, int namecap);

static void on_dir_entry(const DirEntry_Ext *e, void *ctx)
{
    (void)ctx;
    if (e == NULL || npages >= MAX_PAGES)
        return;
    if (e->type == OS32_FILE_TYPE_DIR)
        return;
    if (!has_mgx_ext(e->name))
        return;
    if ((int)strlen(e->name) >= NAME_MAX_LEN)
        return;

    copy_str(pages[npages].name, NAME_MAX_LEN, e->name);
    pages[npages].bitmap   = NULL;
    pages[npages].raw_size = 0;
    pages[npages].failed   = 0;
    npages++;
}

/* dirpath を走査してページ一覧を作り直す。キャッシュは呼び出し側で捨てること。 */
static void rescan_dir(void)
{
    npages = 0;
    cache_full = 0;
    cached_count = 0;
    cached_bytes = 0;
    api->sys_ls(dirpath, (void *)on_dir_entry, NULL);
    sort_pages();
}

/* 名前順 (大文字小文字を無視) に整列。ページ数は高々 MAX_PAGES なので挿入法。 */
static void sort_pages(void)
{
    int i, j;
    Page tmp;

    for (i = 1; i < npages; i++) {
        tmp = pages[i];
        j = i - 1;
        while (j >= 0 && name_cmp_ci(pages[j].name, tmp.name) > 0) {
            pages[j + 1] = pages[j];
            j--;
        }
        pages[j + 1] = tmp;
    }
}

/* ------------------------------------------------------------------------
 *  キャッシュ操作
 * ------------------------------------------------------------------------ */
static void cache_drop(int idx)
{
    if (pages[idx].bitmap != NULL) {
        api->mem_free(pages[idx].bitmap);
        cached_bytes -= pages[idx].raw_size;
        pages[idx].bitmap = NULL;
        cached_count--;
    }
}

/* 現在ページから最も遠いキャッシュを 1 枚捨てる。捨てられたら 1 */
static int cache_evict_farthest(int cur)
{
    int i, best = -1, best_d = -1;

    for (i = 0; i < npages; i++) {
        int d;
        if (pages[i].bitmap == NULL || i == cur)
            continue;
        d = (i > cur) ? (i - cur) : (cur - i);
        if (d > best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best < 0)
        return 0;
    cache_drop(best);
    return 1;
}

/* 1 ページを読み込んで展開しキャッシュへ。
 * 戻り値: 0 = 利用可能, -1 = 失敗, -2 = メモリ不足 (呼び出し側で判断) */
static int page_load(int idx, int allow_evict, int cur)
{
    char path[PATH_MAX_LEN];
    u8 hdrbuf[MGX_HDR_SIZE];
    MgxHeader h;
    u8 *payload;
    u8 *bitmap;
    int fd, rc, n;

    if (pages[idx].bitmap != NULL)
        return 0;
    if (pages[idx].failed)
        return -1;

    build_path(path, PATH_MAX_LEN, pages[idx].name);

    fd = api->sys_open(path, O_RDONLY);
    if (fd < 0) {
        pages[idx].failed = 1;
        return -1;
    }

    n = api->sys_read(fd, hdrbuf, MGX_HDR_SIZE);
    if (n != MGX_HDR_SIZE) {
        api->sys_close(fd);
        pages[idx].failed = 1;
        return -1;
    }

    rc = mgx_parse_header(hdrbuf, MGX_HDR_SIZE, &h);
    if (rc != MGX_OK) {
        api->sys_close(fd);
        pages[idx].failed = 1;
        api->kprintf(ATTR_WHITE, "mgxview: %s: %s\n",
                     pages[idx].name, mgx_errstr(rc));
        return -1;
    }

    /* 展開先を先に確保する。足りなければ遠いページを捨てて再試行。 */
    for (;;) {
        bitmap = (u8 *)api->mem_alloc(h.raw_size);
        if (bitmap != NULL)
            break;
        if (!allow_evict || !cache_evict_farthest(cur)) {
            api->sys_close(fd);
            return -2;
        }
    }

    payload = (u8 *)api->mem_alloc(h.data_size);
    if (payload == NULL) {
        /* 一時バッファすら取れないので、遠いページを捨てて再試行 */
        for (;;) {
            if (!allow_evict || !cache_evict_farthest(cur)) {
                api->mem_free(bitmap);
                api->sys_close(fd);
                return -2;
            }
            payload = (u8 *)api->mem_alloc(h.data_size);
            if (payload != NULL)
                break;
        }
    }

    n = api->sys_read(fd, payload, h.data_size);
    api->sys_close(fd);
    if ((u32)n != h.data_size) {
        api->mem_free(payload);
        api->mem_free(bitmap);
        pages[idx].failed = 1;
        return -1;
    }

    rc = mgx_decode(&h, payload, h.data_size, bitmap, h.raw_size);
    api->mem_free(payload);
    if (rc < 0) {
        api->mem_free(bitmap);
        pages[idx].failed = 1;
        api->kprintf(ATTR_WHITE, "mgxview: %s: %s\n",
                     pages[idx].name, mgx_errstr(rc));
        return -1;
    }

    pages[idx].bitmap     = bitmap;
    pages[idx].hdr        = h;
    pages[idx].raw_size   = h.raw_size;
    pages[idx].plane_size = mgx_plane_size(&h);
    cached_count++;
    cached_bytes += h.raw_size;
    return 0;
}

/* 先読みを 1 枚だけ進める。何かしたら 1、もうすることが無ければ 0。
 * 現在ページの前後へ交互に広げるので、めくる方向がどちらでも効く。 */
static int prefetch_step(int cur)
{
    int d, idx, rc;

    if (cache_full)
        return 0;

    for (d = 0; d <= PREFETCH_RADIUS; d++) {
        int side;
        for (side = 0; side < 2; side++) {
            idx = (side == 0) ? (cur + d) : (cur - d);
            if (d == 0 && side == 1)
                continue;
            if (idx < 0 || idx >= npages)
                continue;
            if (pages[idx].bitmap != NULL || pages[idx].failed)
                continue;

            /* 先読みでは追い出しをしない。空きが尽きたらそこで打ち切る。 */
            rc = page_load(idx, 0, cur);
            if (rc == -2) {
                cache_full = 1;
                return 0;
            }
            return 1;     /* 成功でも失敗でも 1 枚分進んだ */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 *  パレット設定
 *
 *  ヘッダの palette[] が「プレーン値 -> グレー階調 (0..15)」をそのまま
 *  持っているので、bpp=1 と bpp=4 で処理が分かれない。
 *  PC-98 のアナログパレットは各チャンネル 0..15 なので階調 g は (g,g,g)。
 *
 *  戻り値: 余白 (レターボックス) に使うパレット番号
 * ------------------------------------------------------------------------ */
static u8 setup_palette(const MgxHeader *h, int invert)
{
    int i, level;

    for (i = 0; i < MGX_PAL_ENTRIES; i++) {
        level = h->palette[i] & 0x0F;
        if (invert)
            level = 15 - level;
        api->gfx_set_palette(i, (u8)level, (u8)level, (u8)level);
    }
    return (u8)mgx_paper_index(h, invert);
}

/* ------------------------------------------------------------------------
 *  描画
 * ------------------------------------------------------------------------ */
static int draw_page(int idx, int invert)
{
    const MgxHeader *h;
    u8 *planes[4];
    int dx, dy, rc;
    u8 paper;

    if (page_load(idx, 1, idx) != 0)
        return -1;

    h = &pages[idx].hdr;

    /* センタリング (x はバイト境界に丸める: ずれは最大 7px) */
    dx = ((SCREEN_W - h->width) / 2) & ~7;
    dy = (SCREEN_H - h->height) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;

    paper = setup_palette(h, invert);

    gfx_clear(paper);

    planes[0] = gfx_fb.planes[0];
    planes[1] = gfx_fb.planes[1];
    planes[2] = gfx_fb.planes[2];
    planes[3] = gfx_fb.planes[3];

    /* bpp=1..4 のいずれでも同じ経路。階調はパレット表が決める。 */
    rc = mgx_blit_planes(planes, gfx_fb.pitch,
                         gfx_fb.width, gfx_fb.height, dx, dy,
                         pages[idx].bitmap, pages[idx].plane_size,
                         h->bpp, h->width, h->height);
    if (rc != MGX_OK)
        return -1;

    gfx_present();
    api->gfx_present_dirty();
    return 0;
}

/* libos32filer の色番号 (FC_BG=0, FC_TITLE=2, FC_DIR_NAME=3, FC_TEXT=4,
 * FC_PANEL_BG=6, FC_BORDER=7, FC_CURSOR_BG=12, FC_HINT=13) は
 * libos32md の md_palette (programs/libos32md/md_render.c) を前提にしている。
 * mgxview は階調ランプを使うので、ファイラを出す間だけこちらへ差し替える。
 * libos32md をリンクせずに済むよう値をここに持つ。 */
static const u8 filer_palette[16][3] = {
    {  1,  1,  3 },  /* 0: 背景 ダークネイビー */
    { 15, 13,  4 },  /* 1: ゴールド */
    {  5, 12, 15 },  /* 2: タイトル スカイブルー */
    {  4, 14, 10 },  /* 3: ディレクトリ名 エメラルド */
    { 15, 15, 15 },  /* 4: ファイル名 白 */
    { 12,  9, 15 },  /* 5: ラベンダー */
    {  3,  3,  4 },  /* 6: パネル背景 */
    {  7,  7,  8 },  /* 7: 枠線 / サイズ グレー */
    { 15,  7,  6 },  /* 8: サーモン */
    {  5, 13, 15 },  /* 9: シアン */
    { 10, 10, 10 },  /* 10 */
    { 11,  9,  3 },  /* 11 */
    {  2,  2,  4 },  /* 12: カーソル行背景 */
    { 13, 13, 14 },  /* 13: ヒント文字 */
    { 15, 14,  4 },  /* 14 */
    {  6, 10, 13 }   /* 15 */
};

/* ------------------------------------------------------------------------
 *  ファイラで別のページを選ぶ
 *
 *  戻り値: 1 = 選択された (cur を更新), 0 = キャンセル
 * ------------------------------------------------------------------------ */
static int open_filer(int *cur)
{
    char sel[NAME_MAX_LEN];
    const char *path;
    int i;

    for (i = 0; i < 16; i++)
        api->gfx_set_palette(i, filer_palette[i][0],
                             filer_palette[i][1], filer_palette[i][2]);

    /* 前のページのビットが残っていると差し替えたパレットで化けるので消す */
    gfx_clear(0);
    gfx_present();
    api->gfx_present_dirty();

    if (!filer_open(dirpath, ".mgx"))
        return 0;

    path = filer_get_selected_path();
    if (path == NULL || path[0] == '\0')
        return 0;

    /* ディレクトリが変わりうるのでキャッシュを全部捨ててから作り直す */
    for (i = 0; i < npages; i++)
        cache_drop(i);

    split_path(path, sel, NAME_MAX_LEN);    /* dirpath も更新される */
    rescan_dir();

    *cur = 0;
    for (i = 0; i < npages; i++) {
        if (name_cmp_ci(pages[i].name, sel) == 0) {
            *cur = i;
            break;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------
 *  PC-98 デフォルトパレット復元 (vdpview.c と同じ)
 * ------------------------------------------------------------------------ */
static void restore_default_palette(void)
{
    static const u8 default_pal[16][3] = {
        {0,0,0}, {0,0,7}, {7,0,0}, {7,0,7},
        {0,7,0}, {0,7,7}, {7,7,0}, {7,7,7},
        {0,0,0}, {0,0,15},{15,0,0},{15,0,15},
        {0,15,0},{0,15,15},{15,15,0},{15,15,15}
    };
    int i;
    for (i = 0; i < 16; i++)
        api->gfx_set_palette(i, default_pal[i][0],
                             default_pal[i][1], default_pal[i][2]);
}

static void usage(void)
{
    api->kprintf(ATTR_WHITE, "%s",
                 "MGXView v1.0 - Manga Image Viewer for OS32\n");
    api->kprintf(ATTR_WHITE, "%s",
                 "Usage: mgxview [FILE.MGX] [-k] [-i]\n");
    api->kprintf(ATTR_WHITE, "%s",
                 "  (no file) : open all *.MGX in the current directory\n");
    api->kprintf(ATTR_WHITE, "%s",
                 "  -k : keep VRAM on exit\n");
    api->kprintf(ATTR_WHITE, "%s",
                 "  -i : invert black/white\n");
    api->kprintf(ATTR_WHITE, "%s",
                 "Keys: S / RIGHT = next, A / LEFT = prev\n");
    api->kprintf(ATTR_WHITE, "%s",
                 "      F / TAB   = file browser, ESC / q = quit\n");
}

/* ------------------------------------------------------------------------
 *  引数で渡されたパスを「ディレクトリ」と「ファイル名」に割る
 * ------------------------------------------------------------------------ */
static void split_path(const char *path, char *namebuf, int namecap)
{
    int n = (int)strlen(path);
    int cut = -1;
    int i;

    for (i = n - 1; i >= 0; i--) {
        if (path[i] == '/' || path[i] == '\\') {
            cut = i;
            break;
        }
    }

    if (cut < 0) {
        /* ディレクトリ指定なし: カレントディレクトリ */
        copy_str(dirpath, PATH_MAX_LEN, api->sys_getcwd());
        copy_str(namebuf, namecap, path);
    } else {
        int len = (cut == 0) ? 1 : cut;      /* "/foo.mgx" は "/" */
        if (len > PATH_MAX_LEN - 1)
            len = PATH_MAX_LEN - 1;
        for (i = 0; i < len; i++)
            dirpath[i] = path[i];
        dirpath[len] = '\0';
        copy_str(namebuf, namecap, path + cut + 1);
    }
}

/* ======================================================================== */
/*  メインプログラム                                                        */
/* ======================================================================== */
void main(int argc, char **argv, KernelAPI *sys_api)
{
    char startname[NAME_MAX_LEN];
    const char *argfile = NULL;
    int keep_vram = 0;
    int invert = 0;
    int cur = 0;
    int need_redraw = 1;
    int i, ch;

    api = sys_api;
    startname[0] = '\0';

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) {
            keep_vram = 1;
        } else if (strcmp(argv[i], "-i") == 0) {
            invert = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return;
        } else if (argfile == NULL) {
            argfile = argv[i];
        }
    }

    /* --- 走査するディレクトリを決める --- */
    if (argfile != NULL) {
        split_path(argfile, startname, NAME_MAX_LEN);
    } else {
        copy_str(dirpath, PATH_MAX_LEN, api->sys_getcwd());
    }
    if (dirpath[0] == '\0')
        copy_str(dirpath, PATH_MAX_LEN, "/");

    /* --- ディレクトリ内の *.MGX を集める --- */
    rescan_dir();

    if (npages == 0) {
        api->kprintf(ATTR_WHITE, "mgxview: no *.MGX found in %s\n", dirpath);
        usage();
        return;
    }

    /* --- 引数のファイルを開始ページにする --- */
    if (startname[0] != '\0') {
        for (i = 0; i < npages; i++) {
            if (name_cmp_ci(pages[i].name, startname) == 0) {
                cur = i;
                break;
            }
        }
    }

    libos32gfx_init(api);
    filer_init(api);

    /* パレットはページごとに設定する (bpp / palette モードが混在しうる) */

    for (;;) {
        if (need_redraw) {
            draw_page(cur, invert);
            need_redraw = 0;
        }

        ch = api->kbd_trygetchar();
        if (ch < 0) {
            /* 入力待ちの合間にキャッシュを埋める。
             * 埋め終わったら halt して CPU を返す。 */
            if (!prefetch_step(cur))
                api->sys_halt();
            continue;
        }

        if (ch == KEY_ESC || ch == 'q' || ch == 'Q')
            break;

        if (ch == KEY_RIGHT || ch == 's' || ch == 'S' || ch == KEY_SPACE) {
            if (cur + 1 < npages) {
                cur++;
                need_redraw = 1;
            }
        } else if (ch == KEY_LEFT || ch == 'a' || ch == 'A') {
            if (cur > 0) {
                cur--;
                need_redraw = 1;
            }
        } else if (ch == 'f' || ch == 'F' || ch == KEY_TAB) {
            /* 選択してもキャンセルしても画面はファイラで潰れているので描き直す */
            open_filer(&cur);
            if (npages == 0) {
                api->kprintf(ATTR_WHITE, "mgxview: no *.MGX in %s\n", dirpath);
                break;
            }
            need_redraw = 1;
        }
    }

    /* 統計は解放前に控える */
    {
        int  final_pages = cached_count;
        u32  final_kb    = cached_bytes / 1024;

        for (i = 0; i < npages; i++)
            cache_drop(i);
        cached_count = final_pages;
        cached_bytes = final_kb;
    }

    if (!keep_vram) {
        gfx_clear(0);
        gfx_present();
        api->gfx_present_dirty();
    }

    libos32gfx_shutdown();
    restore_default_palette();
    api->tvram_clear();

    api->kprintf(ATTR_WHITE, "mgxview: %d page(s), %d cached (%u KB)\n",
                 npages, cached_count, (unsigned)cached_bytes);
}
