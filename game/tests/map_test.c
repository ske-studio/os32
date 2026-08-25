/* ======================================================================== */
/*  MAP_TEST.C — libos32map テストプログラム                                 */
/*                                                                          */
/*  マップ管理ライブラリの全機能をテストするスイート。                        */
/*  テスト1:  初期化 + DBキャッシュ                                         */
/*  テスト2:  マップロード + タイルデータ                                    */
/*  テスト3:  タイルアクセス (範囲チェック)                                  */
/*  テスト4:  通行判定                                                      */
/*  テスト5:  タイルプロパティ + 化学属性                                    */
/*  テスト6:  イベント検索                                                  */
/*  テスト7:  ワープ                                                        */
/*  テスト8:  エンカウント                                                  */
/*  テスト9:  タイル置換 + 化学属性変更                                     */
/*  テスト10: 矩形通行判定                                                  */
/*  テスト11: マップ切替 (unload→load)                                      */
/*  テスト12: デバッグダンプ                                                */
/* ======================================================================== */

#include "os32api.h"
#include "libos32map.h"
#include "libos32db.h"

extern KernelAPI *kapi;
#define api kapi

/* ---- テストヘルパー ---- */

static int g_total = 0;
static int g_passed = 0;

static void check(const char *label, int cond)
{
    g_total++;
    if (cond) {
        g_passed++;
        api->kprintf(ATTR_GREEN, "  [OK] %s\n", label);
    } else {
        api->kprintf(ATTR_RED, "  [NG] %s\n", label);
    }
}

static void check_eq(const char *label, int got, int expect)
{
    g_total++;
    if (got == expect) {
        g_passed++;
        api->kprintf(ATTR_GREEN, "  [OK] %s = %d\n", label, got);
    } else {
        api->kprintf(ATTR_RED, "  [NG] %s: got %d, expect %d\n",
                     label, got, expect);
    }
}

static void header(const char *title)
{
    api->kprintf(ATTR_CYAN, "\n=== %s ===\n", title);
}

/* ====================================================================== */
/*  テスト用DBセットアップ (メモリDB)                                      */
/* ====================================================================== */

static int setup_test_db(void)
{
    db_handle_t db;
    int rc;

    db = db_open(":memory:");
    if (db < 0) return -1;

    /* テーブル作成 */
    rc = db_exec(db,
        "CREATE TABLE maps("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "width INTEGER NOT NULL, height INTEGER NOT NULL, "
        "layer_count INTEGER NOT NULL DEFAULT 1, "
        "tileset_id INTEGER NOT NULL DEFAULT 0, "
        "bgm_id INTEGER DEFAULT 0)");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE map_tiles("
        "map_id INTEGER NOT NULL, layer INTEGER NOT NULL DEFAULT 0, "
        "tile_data BLOB NOT NULL, PRIMARY KEY(map_id,layer))");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE tilesets("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "tile_file TEXT NOT NULL, tile_count INTEGER NOT NULL)");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE tile_props("
        "tileset_id INTEGER NOT NULL, tile_id INTEGER NOT NULL, "
        "passable INTEGER NOT NULL DEFAULT 1, flags INTEGER DEFAULT 0, "
        "chem_elem INTEGER DEFAULT 0, chem_temp INTEGER DEFAULT 20, "
        "damage INTEGER DEFAULT 0, PRIMARY KEY(tileset_id,tile_id))");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE map_events("
        "id INTEGER PRIMARY KEY, map_id INTEGER NOT NULL, "
        "x INTEGER NOT NULL, y INTEGER NOT NULL, "
        "trigger INTEGER NOT NULL DEFAULT 0, "
        "type INTEGER NOT NULL DEFAULT 0, "
        "param INTEGER DEFAULT 0, script TEXT DEFAULT '')");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE warps("
        "id INTEGER PRIMARY KEY, src_map INTEGER NOT NULL, "
        "src_x INTEGER NOT NULL, src_y INTEGER NOT NULL, "
        "dst_map INTEGER NOT NULL, dst_x INTEGER NOT NULL, "
        "dst_y INTEGER NOT NULL, direction INTEGER DEFAULT 0)");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE encounters("
        "map_id INTEGER NOT NULL, enemy_id INTEGER NOT NULL, "
        "rate INTEGER NOT NULL DEFAULT 10, "
        "min_steps INTEGER DEFAULT 5)");
    if (rc < 0) { db_close(db); return -1; }

    db_close(db);
    return 0;
}

/* ====================================================================== */
/*  テスト1: 初期化                                                        */
/* ====================================================================== */

static void test_init(void)
{
    int rc;

    header("Test 1: Init");

    rc = setup_test_db();
    check("setup_test_db", rc == 0);

    /* :memory: は接続ごとに別DBなのでスキーマなし → init は成功するが
       ロード時にテーブルが見つからない */
    rc = map_init(":memory:");
    check("map_init(:memory:)", rc == 0);

    /* マップ未ロード */
    check("map_current() == NULL", map_current() == (const MapDef *)0);

    map_shutdown();
}

/* ====================================================================== */
/*  テスト2-12: ファイルDB統合テスト                                        */
/* ====================================================================== */

static void test_file_db(void)
{
    int rc;
    const MapDef *m;

    header("Test 2: File DB Load (/db/map.db)");

    rc = map_init("/db/map.db");
    if (rc < 0) {
        api->kprintf(ATTR_RED,
            "  [SKIP] map_init returned %d "
            "(is /db/map.db deployed?)\n", rc);
        return;
    }
    check("map_init(/db/map.db)", rc == 0);

    /* マップ1ロード */
    rc = map_load(1);
    check("map_load(1)", rc == 0);

    m = map_current();
    check("map_current() != NULL", m != (const MapDef *)0);

    if (m) {
        check_eq("map.id", (int)m->id, 1);
        check_eq("map.width", (int)m->width, 24);
        check_eq("map.height", (int)m->height, 24);
        check_eq("map.layer_count", (int)m->layer_count, 1);
        check_eq("map.tileset_id", (int)m->tileset_id, 0);
        api->kprintf(ATTR_WHITE, "  name=%s\n", m->name);
    }

    /* --- テスト3: タイルアクセス --- */
    header("Test 3: Tile Access");
    if (m) {
        u16 t;

        /* 外周は壁(1) */
        t = map_get_tile(0, 0, 0);
        check_eq("tile(0,0,0) = wall(1)", (int)(t & 0x3FF), 1);

        /* 内部は草(0) */
        t = map_get_tile(0, 5, 5);
        check_eq("tile(0,5,5) = grass(0)", (int)(t & 0x3FF), 0);

        /* 池は水(2) */
        t = map_get_tile(0, 10, 10);
        check_eq("tile(0,10,10) = water(2)", (int)(t & 0x3FF), 2);

        /* 木(3) */
        t = map_get_tile(0, 3, 4);
        check_eq("tile(0,3,4) = tree(3)", (int)(t & 0x3FF), 3);

        /* 氷(4) */
        t = map_get_tile(0, 19, 6);
        check_eq("tile(0,19,6) = ice(4)", (int)(t & 0x3FF), 4);

        /* ワープ(6) */
        t = map_get_tile(0, 22, 22);
        check_eq("tile(0,22,22) = warp(6)", (int)(t & 0x3FF), 6);

        /* 範囲外 */
        t = map_get_tile(0, -1, 0);
        check_eq("tile(0,-1,0) = 0", (int)t, 0);
        t = map_get_tile(0, 24, 0);
        check_eq("tile(0,24,0) = 0", (int)t, 0);
        t = map_get_tile(1, 0, 0);
        check_eq("tile(1,0,0) = 0 (no layer1)", (int)t, 0);
    }

    /* --- テスト4: 通行判定 --- */
    header("Test 4: Passability");
    if (m) {
        check("passable(5,5) grass",  map_is_passable(5, 5));
        check("!passable(0,0) wall", !map_is_passable(0, 0));
        check("passable(10,10) water(p=2)", map_is_passable(10, 10));
        check("!passable(3,4) tree", !map_is_passable(3, 4));
        check("passable(19,6) ice",  map_is_passable(19, 6));
        check("!passable(-1,0) OOB", !map_is_passable(-1, 0));
    }

    /* --- テスト5: タイルプロパティ --- */
    header("Test 5: Tile Properties");
    if (m) {
        const TileProp *prop;

        /* 壁 tile 1 */
        prop = map_get_prop(0, 0);
        check("prop(0,0) != NULL", prop != (const TileProp *)0);
        if (prop) {
            check_eq("wall passable", (int)prop->passable, 0);
        }

        /* 草 tile 0 */
        prop = map_get_prop(5, 5);
        check("prop(5,5) != NULL", prop != (const TileProp *)0);
        if (prop) {
            check_eq("grass passable", (int)prop->passable, 1);
            check("grass chem=GRASS(0x40)",
                  (prop->chem_elements & 0x40) != 0);
        }

        /* 水 tile 2 */
        prop = map_get_prop(10, 10);
        if (prop) {
            check("water chem=WATER(0x02)",
                  (prop->chem_elements & 0x02) != 0);
            check_eq("water temp", (int)prop->chem_temperature, 15);
        }

        /* 氷 tile 4 */
        prop = map_get_prop(19, 6);
        if (prop) {
            check("ice flags has SLIPPERY",
                  (prop->flags & MAP_TFLAG_SLIPPERY) != 0);
            check("ice chem=ICE(0x08)",
                  (prop->chem_elements & 0x08) != 0);
        }

        /* 化学属性取得 */
        {
            u32 chem = map_get_chem_elements(3, 4);
            check("wood chem=WOOD(0x04)", (chem & 0x04) != 0);
        }
    }

    /* --- テスト6: イベント検索 --- */
    header("Test 6: Events");
    if (m) {
        const MapEvent *evt;

        check("event_count > 0", map__event_count() > 0);
        api->kprintf(ATTR_WHITE, "  events cached: %d\n",
                     map__event_count());

        /* 宝箱 (5,5 ACTION) */
        evt = map_get_event(5, 5, MAP_TRIG_ACTION);
        check("treasure at (5,5)", evt != (const MapEvent *)0);
        if (evt) {
            check_eq("treasure type", (int)evt->type, MAP_EVT_TREASURE);
            check_eq("treasure param", (int)evt->param, 100);
        }

        /* NPC (8,4 ACTION) */
        evt = map_get_event(8, 4, MAP_TRIG_ACTION);
        check("NPC at (8,4)", evt != (const MapEvent *)0);
        if (evt) {
            check_eq("NPC type", (int)evt->type, MAP_EVT_NPC);
        }

        /* ワープ (22,22 STEP) */
        evt = map_get_event(22, 22, MAP_TRIG_STEP);
        check("warp at (22,22)", evt != (const MapEvent *)0);
        if (evt) {
            check_eq("warp type", (int)evt->type, MAP_EVT_WARP);
            check_eq("warp param (dst=2)", (int)evt->param, 2);
        }

        /* 存在しないイベント */
        evt = map_get_event(15, 15, MAP_TRIG_STEP);
        check("no event at (15,15)", evt == (const MapEvent *)0);
    }

    /* --- テスト7: ワープ --- */
    header("Test 7: Warps");
    if (m) {
        check("warp_count > 0", map__warp_count() > 0);
        api->kprintf(ATTR_WHITE, "  warps cached: %d\n",
                     map__warp_count());
    }

    /* --- テスト8: エンカウント --- */
    header("Test 8: Encounters");
    if (m) {
        check("encounter_count > 0", map__encounter_count() > 0);
        api->kprintf(ATTR_WHITE, "  encounters cached: %d\n",
                     map__encounter_count());

        /* 歩数カウンタリセット */
        map_reset_steps();
        /* 大量歩行でエンカウント発生するか試行 */
        {
            int trial;
            int hit = 0;
            for (trial = 0; trial < 100; trial++) {
                map_check_step(5, 5);
                if (map_check_encounter() > 0) {
                    hit = 1;
                    break;
                }
            }
            /* エンカウントは確率依存なので失敗してもOK */
            api->kprintf(ATTR_WHITE,
                "  encounter hit in %d trials: %s\n",
                trial + 1, hit ? "YES" : "NO (probabilistic)");
        }
    }

    /* --- テスト9: タイル置換 + 化学属性変更 --- */
    header("Test 9: Tile Replace + Chem");
    if (m) {
        u16 before, after;

        /* 草タイルを壁に置換 */
        before = map_get_tile(0, 5, 5);
        map_replace_tile(0, 5, 5, 1);
        after = map_get_tile(0, 5, 5);

        check_eq("before replace", (int)(before & 0x3FF), 0);
        check_eq("after replace", (int)(after & 0x3FF), 1);
        check("now impassable", !map_is_passable(5, 5));

        /* 元に戻す */
        map_replace_tile(0, 5, 5, 0);

        /* 範囲外置換 (クラッシュしない) */
        map_replace_tile(0, -1, 0, 99);
        map_replace_tile(3, 0, 0, 99);
    }

    /* --- テスト10: 矩形通行判定 --- */
    header("Test 10: Rect Passability");
    if (m) {
        /* 内部の草エリア (木を避ける): 通行可能 */
        check("rect(5,2,3,3) passable",
              map_rect_passable(5, 2, 3, 3));
        /* 壁を含むエリア: 通行不可 */
        check("rect(0,0,3,3) impassable",
              !map_rect_passable(0, 0, 3, 3));
    }

    /* --- テスト11: マップ切替 --- */
    header("Test 11: Map Switch");
    {
        int rc2;

        /* マップ2ロード */
        rc2 = map_load(2);
        if (rc2 == 0) {
            const MapDef *m2 = map_current();
            check("map2 loaded", m2 != (const MapDef *)0);
            if (m2) {
                check_eq("map2.id", (int)m2->id, 2);
                check_eq("map2.width", (int)m2->width, 16);
                check_eq("map2.height", (int)m2->height, 16);
            }

            /* マップ1に戻す */
            rc2 = map_load(1);
            check("back to map1", rc2 == 0);
            m = map_current();
            if (m) {
                check_eq("map1.id again", (int)m->id, 1);
            }
        } else {
            api->kprintf(ATTR_YELLOW,
                "  [SKIP] map_load(2) returned %d\n", rc2);
        }
    }

    /* --- テスト12: デバッグダンプ --- */
    header("Test 12: Debug Dump");
    map_debug_dump();

    /* --- カメラ --- */
    header("Test 13: Camera");
    {
        i16 cx, cy;
        map_set_camera(12, 12);
        map_get_camera(&cx, &cy);
        check_eq("camera_x", (int)cx, 12);
        check_eq("camera_y", (int)cy, 12);
    }

    /* --- 存在しないマップ --- */
    header("Test 14: Non-existent Map");
    {
        int rc3;
        rc3 = map_load(999);
        check("map_load(999) fails", rc3 < 0);
    }

    map_shutdown();
}

/* ====================================================================== */
/*  エントリポイント                                                       */
/* ====================================================================== */

int main(int argc, char **argv, KernelAPI *k)
{
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_CYAN, "map_test: libos32map test suite\n");
    api->kprintf(ATTR_CYAN, "KAPI version: %d\n", kapi->version);

    /* Phase 1: 基本テスト */
    test_init();

    /* Phase 1: ファイルDB統合テスト */
    test_file_db();

    /* サマリ */
    api->kprintf(ATTR_CYAN, "\n=== Result: %d/%d passed ===\n",
                 g_passed, g_total);
    if (g_passed == g_total) {
        api->kprintf(ATTR_GREEN, "All tests passed!\n");
    } else {
        api->kprintf(ATTR_RED, "%d test(s) failed.\n", g_total - g_passed);
    }
    return 0;
}
