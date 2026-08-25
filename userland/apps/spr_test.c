#include "os32api.h"
#include "libos32gfx.h"

/* sprintf用 */
#define SPRINTF_MAX 64
extern int sprintf(char *str, const char *format, ...);

/* GFX_Surface用のプレーンバッファをグローバルに確保 (スタックオーバーフロー回避) */
u8 buf_16[16 * 2 * 4];       /* 16x16: 2Bytes * 16Lines * 4Planes = 128 Bytes */
u8 buf_32[32 * 4 * 4];       /* 32x32: 4Bytes * 32Lines * 4Planes = 512 Bytes */
u8 buf_64[64 * 8 * 4];       /* 64x64: 8Bytes * 64Lines * 4Planes = 2048 Bytes */
u8 buf_128[128 * 16 * 4];    /* 128x128: 16Bytes * 128Lines * 4Planes = 8192 Bytes */

/* 矩形の枠線＋市松模様のテスト用サーフェスを構築する */
void setup_surf(GFX_Surface *surf, u8 *buf, int size) {
    int i, p;
    int bytes_per_line = size / 8;
    int total_bytes = bytes_per_line * size;

    surf->w = size;
    surf->h = size;
    surf->pitch = bytes_per_line;
    surf->_pool_idx = -1;

    for (p = 0; p < 4; p++) {
        surf->planes[p] = buf + (p * total_bytes);
    }

    /* 各プレーンに色データを書き込む */
    for (i = 0; i < total_bytes; i++) {
        int x_byte = i % bytes_per_line;
        int y = i / bytes_per_line;
        u8 val = 0;

        /* 四角の枠とX状のクロス模様を描く */
        if (y == 0 || y == size - 1) {
            val = 0xFF; /* 上下枠 */
        } else if (x_byte == 0) {
            val = 0x80; /* 左枠 (先頭ビット) */
        } else if (x_byte == bytes_per_line - 1) {
            val = 0x01; /* 右枠 (末尾ビット) */
        } else {
            /* 内部は簡易的なドット斜め線 */
            int px = x_byte * 8;
            if ((px + y) % 4 == 0) val = 0xAA;
            else val = 0x00;
        }

        /* サイズごとに色を分ける */
        if (size == 16) {
            /* 白 (全プレーンON) */
            surf->planes[0][i] = val; surf->planes[1][i] = val; surf->planes[2][i] = val; surf->planes[3][i] = val;
        } else if (size == 32) {
            /* 赤 (プレーン1+3) */
            surf->planes[0][i] = 0; surf->planes[1][i] = val; surf->planes[2][i] = 0; surf->planes[3][i] = val;
        } else if (size == 64) {
            /* 緑 (プレーン2+3) */
            surf->planes[0][i] = 0; surf->planes[1][i] = 0; surf->planes[2][i] = val; surf->planes[3][i] = val;
        } else /* 128 */ {
            /* 黄色 (プレーン1+2+3) */
            surf->planes[0][i] = 0; surf->planes[1][i] = val; surf->planes[2][i] = val; surf->planes[3][i] = val;
        }
    }
}

typedef struct {
    GFX_Sprite *spr;
    int x, y;
    int vx, vy;
    int old_x, old_y;   /* 前回の座標 */
} Node;

#define NUM_NODES 4
Node nodes[NUM_NODES];

#define VP_X ((640 - 320) / 2)
#define VP_Y ((400 - 320) / 2)
#define VP_W 320
#define VP_H 320

static void draw_fps_bar(KernelAPI *api, int fps)
{
    int bw;
    u8 c;

    /* バー背景クリア */
    gfx_fill_rect(VP_X + 1, VP_Y + VP_H + 7, 300, 8, 0);

    /* FPSバー */
    bw = fps * 5;
    if (bw > 300) bw = 300;
    if (bw > 0) {
        if (fps >= 55) c = 10;       /* 緑 */
        else if (fps >= 30) c = 14;  /* 黄 */
        else c = 12;                  /* 赤 */
        gfx_fill_rect(VP_X + 1, VP_Y + VP_H + 7, bw, 8, c);
    }

    /* 30fps/60fps目盛り */
    gfx_vline(VP_X + 150, VP_Y + VP_H + 7, 8, 8);
    gfx_vline(VP_X + 300, VP_Y + VP_H + 7, 8, 7);

    /* FPSバー領域はdirtyリストに登録 */
    api->gfx_add_dirty_rect(VP_X, VP_Y + VP_H + 6, 304, 12);
}

void main(int argc, char **argv, KernelAPI *api) {
    GFX_Surface surf16, surf32, surf64, surf128;
    u32 last_tick, fps_tick;
    int fps = 0, fps_frames = 0;
    char fps_buf[SPRINTF_MAX];
    int i;

    api->kprintf(0xE1, "Starting Size Test Demo (High Perf)... (Press any key to exit)\n");
    libos32gfx_init(api);

    setup_surf(&surf16, buf_16, 16);
    setup_surf(&surf32, buf_32, 32);
    setup_surf(&surf64, buf_64, 64);
    setup_surf(&surf128, buf_128, 128);

    nodes[0].spr = gfx_create_sprite(&surf16, 0);
    nodes[1].spr = gfx_create_sprite(&surf32, 0);
    nodes[2].spr = gfx_create_sprite(&surf64, 0);
    nodes[3].spr = gfx_create_sprite(&surf128, 0);

    for (i = 0; i < NUM_NODES; i++) {
        if (!nodes[i].spr) {
            libos32gfx_shutdown();
            api->kprintf(0x41, "Failed to create sprite %d.\n", i);
            return;
        }
    }

    /* 初期位置と速度 */
    nodes[0].x = 10;  nodes[0].y = 10;  nodes[0].vx = 2; nodes[0].vy = 2;
    nodes[1].x = 100; nodes[1].y = 50;  nodes[1].vx = -1; nodes[1].vy = 2;
    nodes[2].x = 200; nodes[2].y = 150; nodes[2].vx = 2; nodes[2].vy = -1;
    nodes[3].x = 300; nodes[3].y = 200; nodes[3].vx = -1; nodes[3].vy = -1;
    
    for (i = 0; i < NUM_NODES; i++) {
        nodes[i].old_x = nodes[i].x;
        nodes[i].old_y = nodes[i].y;
    }

    /* 最初に一度だけ全画面背景を描画し、全画面VRAM転送を行う */
    gfx_clear(1); /* 青 */
    gfx_fill_rect(OS32_GFX_WIDTH/2 - 50, OS32_GFX_HEIGHT/2 - 50, 100, 100, 4);

    /* ビューポートの枠線（白）を描く */
    gfx_rect((640 - 320) / 2 - 1, (400 - 320) / 2 - 1, 322, 322, 15);
    gfx_present();

    /* 320x320のビューポートを設定する (以後の描画はこの中でクリップされる) */
    /* api->gfx_set_viewport((640 - 320) / 2, (400 - 320) / 2, 320, 320); */

    /* それぞれの初期位置の背景を退避してから、初回のスプライトを描画 */
    for (i = 0; i < NUM_NODES; i++) gfx_save_rect(nodes[i].x, nodes[i].y, nodes[i].spr->w, nodes[i].spr->h, nodes[i].spr->bg_buf);
    for (i = 0; i < NUM_NODES; i++) gfx_draw_sprite(nodes[i].x, nodes[i].y, nodes[i].spr);
    for (i = 0; i < NUM_NODES; i++) api->gfx_add_dirty_rect(nodes[i].x, nodes[i].y, nodes[i].spr->w, nodes[i].spr->h);

    /* フォント設定 (等倍) */
    kcg_set_scale(1);

    last_tick = api->get_tick();
    fps_tick = last_tick;

    /* メインループ (全画面クリアや全画面更新は行わない) */
    while (1) {
        if (api->kbd_trygetchar() != -1) break;

        /* フェーズ1: すべてのスプライトを消す (古い位置の背景を書き戻す) */
        for (i = 0; i < NUM_NODES; i++) {
            gfx_restore_rect(nodes[i].old_x, nodes[i].old_y, nodes[i].spr->w, nodes[i].spr->h, nodes[i].spr->bg_buf);
        }

        /* フェーズ2: 全スプライトの座標移動 */
        for (i = 0; i < NUM_NODES; i++) {
            nodes[i].x += nodes[i].vx;
            nodes[i].y += nodes[i].vy;

            int vp_x = (640 - 320) / 2;
            int vp_y = (400 - 320) / 2;

            if (nodes[i].x <= vp_x) { nodes[i].x = vp_x; nodes[i].vx = -nodes[i].vx; }
            else if (nodes[i].x + nodes[i].spr->w >= vp_x + 320) { nodes[i].x = vp_x + 320 - nodes[i].spr->w - 1; nodes[i].vx = -nodes[i].vx; }

            if (nodes[i].y <= vp_y) { nodes[i].y = vp_y; nodes[i].vy = -nodes[i].vy; }
            else if (nodes[i].y + nodes[i].spr->h >= vp_y + 320) { nodes[i].y = vp_y + 320 - nodes[i].spr->h - 1; nodes[i].vy = -nodes[i].vy; }
        }

        /* フェーズ3: スプライトが描かれる"前"の新しい背景を順番に退避する */
        for (i = 0; i < NUM_NODES; i++) {
            gfx_save_rect(nodes[i].x, nodes[i].y, nodes[i].spr->w, nodes[i].spr->h, nodes[i].spr->bg_buf);
        }

        /* フェーズ4: スプライトの描画 */
        for (i = 0; i < NUM_NODES; i++) {
            gfx_draw_sprite(nodes[i].x, nodes[i].y, nodes[i].spr);
        }

        /* フェーズ5: 消した部分(古い領域)と描いた部分(新しい領域)だけをVRAMに部分転送する */
        for (i = 0; i < NUM_NODES; i++) {
            /* VRAMへ転送キューへ追加 */
            api->gfx_add_dirty_rect(nodes[i].old_x, nodes[i].old_y, nodes[i].spr->w, nodes[i].spr->h);
            api->gfx_add_dirty_rect(nodes[i].x, nodes[i].y, nodes[i].spr->w, nodes[i].spr->h);
            
            nodes[i].old_x = nodes[i].x;
            nodes[i].old_y = nodes[i].y;
        }

        fps_frames++;

        /* FPS計測 (1秒ごと) */
        if (api->get_tick() - fps_tick >= 100) {
            fps = fps_frames;
            fps_frames = 0;
            fps_tick = api->get_tick();
            
            draw_fps_bar(api, fps);

            /* FPS数値テキスト */
            gfx_fill_rect(VP_X + 310, VP_Y + VP_H + 7, 48, 16, 0);
            sprintf(fps_buf, "%d fps", fps);
            kcg_draw_utf8(VP_X + 310, VP_Y + VP_H + 7, fps_buf, 15, 0);
            api->gfx_add_dirty_rect(VP_X + 310, VP_Y + VP_H + 6, 50, 20);
        }

        api->gfx_present_dirty();

        /* フレートレート調整: CPUを休ませながら100Hz周期に同期 */
        while (api->get_tick() == last_tick) {
            api->sys_halt();
        }
        last_tick = api->get_tick();
    }

    for (i = 0; i < NUM_NODES; i++) if (nodes[i].spr) gfx_free_sprite(nodes[i].spr);
    libos32gfx_shutdown();
    api->kprintf(0xE1, "Size Test Demo Exited.\n");
}
