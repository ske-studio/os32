/* ======================================================================== */
/*  DEMO1.C — libos32gfx スプライト負荷ベンチマーク                          */
/*                                                                          */
/*  5秒ごとにスプライトを追加し、FPSが20を下回ったら終了。                    */
/*  結果を 0:/DEMO1.LOG として出力する。                                     */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "os32api.h"
#include "libos32gfx.h"

/* ======== 定数 ======== */
#define STAGE_W    320
#define STAGE_H    200
#define STAGE_X    ((640 - STAGE_W) / 2)
#define STAGE_Y    ((400 - STAGE_H) / 2)

#define SPRITE_SIZE    16
#define SPRITES_PER_STEP 4
#define MAX_SPRITES    60   /* 16x16スプライト上限 (プール制約) */
#define PHASE_TICKS    500  /* 5秒 = 500 ticks (100Hz) */
#define FPS_THRESHOLD  20

/* ======== 簡易乱数 ======== */
static unsigned long _seed = 7654321UL;
static int rnd(int max)
{
    _seed = _seed * 1103515245UL + 12345UL;
    return (int)((_seed >> 16) % (unsigned long)max);
}

/* ======== エンティティ管理 ======== */
typedef struct {
    int x, y;
    int dx, dy;
    GFX_Sprite *spr;
    int active;
} Entity;

static Entity *ents = 0;
static int ent_count = 0;

/* ======== ログ用 ======== */
typedef struct {
    int phase;
    int sprites;
    int fps;
} PhaseResult;

#define MAX_PHASES 32
static PhaseResult *results = 0;
static int result_count = 0;

/* ======== スプライト生成 ======== */
static GFX_Sprite *make_sprite(u8 color)
{
    GFX_Surface *surf = gfx_create_surface(SPRITE_SIZE, SPRITE_SIZE);
    GFX_Sprite *spr;
    if (!surf) return 0;

    gfx_surface_clear(surf, 0);
    gfx_surface_fill_rect(surf, 0, 0, 16, 16, color);
    gfx_surface_fill_rect(surf, 0, 0, 16, 1, 15);
    gfx_surface_fill_rect(surf, 0, 15, 16, 1, 15);
    gfx_surface_fill_rect(surf, 0, 0, 1, 16, 15);
    gfx_surface_fill_rect(surf, 15, 0, 1, 16, 15);
    gfx_surface_fill_rect(surf, 4, 7, 8, 1, 15);
    gfx_surface_fill_rect(surf, 7, 4, 1, 8, 15);

    spr = gfx_create_sprite(surf, 0);
    gfx_free_surface(surf);
    return spr;
}

static int add_sprites(int count)
{
    static const u8 colors[] = {9, 10, 12, 14, 11, 13, 1, 2};
    int i;
    int added = 0;

    for (i = 0; i < count && ent_count < MAX_SPRITES; i++) {
        GFX_Sprite *spr = make_sprite(colors[ent_count % 8]);
        if (!spr) break;

        ents[ent_count].spr = spr;
        ents[ent_count].x = STAGE_X + rnd(STAGE_W - SPRITE_SIZE);
        ents[ent_count].y = STAGE_Y + rnd(STAGE_H - SPRITE_SIZE);
        ents[ent_count].dx = (rnd(2) ? 2 : -2);
        ents[ent_count].dy = (rnd(2) ? 1 : -1);
        ents[ent_count].active = 1;

        /* 初回背景退避 */
        gfx_sprite_save_bg(ents[ent_count].x, ents[ent_count].y, spr);
        ent_count++;
        added++;
    }
    return added;
}

/* ======== ログ出力 ======== */
static void write_log(KernelAPI *api)
{
    int fd;
    int i;
    char buf[128];
    int len;

    fd = api->sys_open("0:/DEMO1.LOG", KAPI_O_CREAT | KAPI_O_WRONLY | KAPI_O_TRUNC);
    if (fd < 0) return;

    len = sprintf(buf, "=== OS32 Sprite Benchmark ===\r\n\r\n");
    api->sys_write(fd, buf, len);

    len = sprintf(buf, "Phase  Sprites  FPS\r\n");
    api->sys_write(fd, buf, len);

    len = sprintf(buf, "-----  -------  ---\r\n");
    api->sys_write(fd, buf, len);

    for (i = 0; i < result_count; i++) {
        len = sprintf(buf, "  %2d      %3d   %2d\r\n",
                      results[i].phase, results[i].sprites, results[i].fps);
        api->sys_write(fd, buf, len);
    }

    len = sprintf(buf, "\r\nResult: %d sprites at %d FPS (threshold: %d)\r\n",
                  results[result_count - 1].sprites,
                  results[result_count - 1].fps,
                  FPS_THRESHOLD);
    api->sys_write(fd, buf, len);

    api->sys_close(fd);
}

/* ======== メインプログラム ======== */
void main(int argc, char **argv, KernelAPI *api)
{
    int phase;
    int running;
    char buf[64];
    int len;

    libos32gfx_init(api);

    /* 配列をヒープに確保 (BSS削減のため) */
    ents = (Entity *)api->mem_alloc(sizeof(Entity) * MAX_SPRITES);
    results = (PhaseResult *)api->mem_alloc(sizeof(PhaseResult) * MAX_PHASES);
    if (!ents || !results) return;

    /* 背景描画 */
    gfx_clear(0);
    gfx_rect(STAGE_X - 1, STAGE_Y - 1, STAGE_W + 2, STAGE_H + 2, 7);
    kcg_set_scale(1);
    kcg_draw_utf8(STAGE_X, STAGE_Y - 16, "Sprite Benchmark", 14, 0);
    gfx_present();

    /* 初期スプライト追加 */
    add_sprites(SPRITES_PER_STEP);

    running = 1;
    phase = 0;

    while (running) {
        u32 phase_start;
        int phase_frames;
        int fps;
        u32 last_tick;

        phase++;
        phase_start = api->get_tick();
        phase_frames = 0;
        last_tick = phase_start;

        /* 5秒間のフレーム計測ループ */
        while (api->get_tick() - phase_start < PHASE_TICKS) {
            int i;
            int ch;

            /* ESCで中断 */
            while ((ch = api->kbd_trygetchar()) >= 0) {
                if (ch == 0x1B) { running = 0; goto phase_end; }
            }

            /* 背景復旧 */
            for (i = ent_count - 1; i >= 0; i--) {
                gfx_sprite_restore_bg(ents[i].x, ents[i].y, ents[i].spr);
            }

            /* 座標更新 */
            for (i = 0; i < ent_count; i++) {
                ents[i].x += ents[i].dx;
                ents[i].y += ents[i].dy;
                if (ents[i].x <= STAGE_X || ents[i].x >= STAGE_X + STAGE_W - SPRITE_SIZE)
                    ents[i].dx = -ents[i].dx;
                if (ents[i].y <= STAGE_Y || ents[i].y >= STAGE_Y + STAGE_H - SPRITE_SIZE)
                    ents[i].dy = -ents[i].dy;
            }

            /* 背景退避 + 描画 */
            for (i = 0; i < ent_count; i++) {
                gfx_sprite_save_bg(ents[i].x, ents[i].y, ents[i].spr);
            }
            for (i = 0; i < ent_count; i++) {
                gfx_draw_sprite(ents[i].x, ents[i].y, ents[i].spr);
            }

            /* VRAM転送 */
            api->gfx_present_dirty();

            phase_frames++;

            /* 次のtickまで待機 */
            while (api->get_tick() == last_tick) {
                api->sys_halt();
            }
            last_tick = api->get_tick();
        }
phase_end:

        /* FPS計算 (5秒間) */
        fps = phase_frames / 5;

        /* 結果記録 */
        if (result_count < MAX_PHASES) {
            results[result_count].phase = phase;
            results[result_count].sprites = ent_count;
            results[result_count].fps = fps;
            result_count++;
        }

        /* 画面にFPS表示 */
        gfx_fill_rect(STAGE_X, STAGE_Y + STAGE_H + 8, 250, 16, 0);
        len = sprintf(buf, "Phase %d: %d sprites, %d fps", phase, ent_count, fps);
        kcg_draw_utf8(STAGE_X, STAGE_Y + STAGE_H + 8, buf, 10, 0);
        api->gfx_present_rect(STAGE_X, STAGE_Y + STAGE_H + 8, 250, 16);

        /* 終了判定 */
        if (!running || fps < FPS_THRESHOLD || ent_count >= MAX_SPRITES) {
            running = 0;
        } else {
            /* スプライト追加 */
            if (add_sprites(SPRITES_PER_STEP) == 0) {
                running = 0; /* プール不足 */
            }
        }
    }

    /* ログファイル出力 */
    write_log(api);

    /* 結果表示を3秒維持 */
    {
        u32 wait_start = api->get_tick();
        while (api->get_tick() - wait_start < 300) {
            api->sys_halt();
        }
    }

    /* クリーンアップ */
    gfx_clear(0);
    gfx_present();
    libos32gfx_shutdown();
    api->tvram_clear();
}
