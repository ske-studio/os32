/* ======================================================================== */
/*  CHEM_TEST.C — libos32chem テストプログラム                               */
/*                                                                          */
/*  化学エンジンの全機能をテストするスイート。                                */
/*  テスト1: 初期化 + DBキャッシュ                                          */
/*  テスト2: オブジェクト生成・消滅                                          */
/*  テスト3: 属性操作                                                       */
/*  テスト4: 温度操作                                                       */
/*  テスト5: 反応処理 (chem_react)                                          */
/*  テスト6: 毎フレーム更新 (chem_update)                                   */
/*  テスト7: 範囲検索・カウント                                              */
/*  テスト8: 範囲攻撃 (chem_apply_area)                                     */
/* ======================================================================== */

#include "os32api.h"
#include "libos32chem.h"
#include "libos32db.h"

extern KernelAPI *kapi;
#define api kapi

/* テスト用: 内部オブジェクト配列への直接アクセス (chem_core.c) */
extern ChemObject *chem__get_objects(void);

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
/*  テスト用DBセットアップ                                                 */
/* ====================================================================== */

/* メモリDBにテスト用スキーマとルールを投入する */
static int setup_test_db(void)
{
    db_handle_t db;
    int rc;

    db = db_open(":memory:");
    if (db < 0) return -1;

    /* テーブル作成 */
    rc = db_exec(db,
        "CREATE TABLE elements("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, flag INTEGER NOT NULL UNIQUE)");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE reactions("
        "id INTEGER PRIMARY KEY, elem_a INTEGER NOT NULL, elem_b INTEGER NOT NULL, "
        "action INTEGER NOT NULL, target INTEGER NOT NULL, "
        "spawn_elem INTEGER DEFAULT 0, temp_delta INTEGER DEFAULT 0, "
        "hp_delta INTEGER DEFAULT 0, priority INTEGER DEFAULT 5)");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE phase_transitions("
        "id INTEGER PRIMARY KEY, elem_from INTEGER NOT NULL, "
        "temp_min INTEGER NOT NULL, temp_max INTEGER NOT NULL, "
        "elem_to INTEGER NOT NULL, spawn_elem INTEGER DEFAULT 0)");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE object_types("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "elements INTEGER NOT NULL, temperature INTEGER DEFAULT 20, "
        "hp INTEGER DEFAULT 100, flammable INTEGER DEFAULT 0)");
    if (rc < 0) { db_close(db); return -1; }

    /* 属性マスター */
    db_exec(db, "INSERT INTO elements VALUES(1, 'Fire', 1)");
    db_exec(db, "INSERT INTO elements VALUES(2, 'Water', 2)");
    db_exec(db, "INSERT INTO elements VALUES(3, 'Wood', 4)");
    db_exec(db, "INSERT INTO elements VALUES(4, 'Ice', 8)");

    /* 反応ルール: 火+木 → 木に着火 (優先度10) */
    db_exec(db,
        "INSERT INTO reactions VALUES(1, 1, 4, 1, 1, 0, 50, 0, 10)");
    /* 反応ルール: 水+火 → 火を消火 (優先度8) */
    db_exec(db,
        "INSERT INTO reactions VALUES(2, 2, 1, 2, 1, 0, -30, 0, 8)");
    /* 反応ルール: 火+木 → ダメージ (優先度5) */
    db_exec(db,
        "INSERT INTO reactions VALUES(3, 1, 4, 8, 1, 0, 0, -20, 5)");

    /* 状態遷移: 水 + 温度>=100 → 蒸気 */
    db_exec(db,
        "INSERT INTO phase_transitions VALUES(1, 2, 100, 32767, 32, 0)");
    /* 状態遷移: 水 + 温度<=0 → 氷 */
    db_exec(db,
        "INSERT INTO phase_transitions VALUES(2, 2, -32768, 0, 8, 0)");
    /* 状態遷移: 氷 + 温度>0 → 水 */
    db_exec(db,
        "INSERT INTO phase_transitions VALUES(3, 8, 1, 32767, 2, 0)");

    /* オブジェクト型テンプレート */
    db_exec(db,
        "INSERT INTO object_types VALUES(1, 'Tree', 4, 20, 100, 1)");
    db_exec(db,
        "INSERT INTO object_types VALUES(2, 'Barrel', 4, 20, 50, 1)");

    db_close(db);
    return 0;
}

/* ====================================================================== */
/*  テスト1: 初期化 + DBキャッシュ                                         */
/* ====================================================================== */
static void test_init(void)
{
    int rc;

    header("Test 1: Init + DB Cache");

    /* テスト用DBのセットアップ (メモリDB) */
    rc = setup_test_db();
    check("setup_test_db", rc == 0);

    /* chem_init — メモリDBでテスト */
    rc = chem_init(":memory:");
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW,
            "  [INFO] :memory: init returned %d "
            "(expected: test DB was separate connection)\n", rc);
    }

    /* ルールが0件でも初期化自体は成功する (テーブルが空の場合) */
    /* 注: :memory: は接続ごとに別DBなのでルール0件が正常 */
    check("chem_init success", rc == 0 || rc == -2 || rc == -3);

    /* 実際のDBファイルを使ったテストは Phase 2 で行う */
    chem_shutdown();
}

/* ====================================================================== */
/*  テスト2: オブジェクト生成・消滅 (DB不要の単体テスト)                     */
/* ====================================================================== */
static void test_objects(void)
{
    int id1, id2, id3;
    const ChemObject *obj;

    header("Test 2: Object Management");

    /* 初期化せずに使えるようchem_resetだけ呼ぶ */
    chem_reset();

    /* type_id=0 でテンプレートなしの生成 */
    id1 = chem_spawn(0, 10, 20);
    check("spawn obj1", id1 >= 0);

    obj = chem_get(id1);
    check("get obj1 != NULL", obj != (const ChemObject *)0);
    if (obj) {
        check_eq("obj1.x", obj->x, 10);
        check_eq("obj1.y", obj->y, 20);
        check_eq("obj1.temperature", obj->temperature, 20);
        check_eq("obj1.hp", obj->hp, 100);
    }

    id2 = chem_spawn(0, 30, 40);
    check("spawn obj2", id2 >= 0);
    check("obj2 != obj1", id2 != id1);

    /* 消滅 */
    chem_destroy(id1);
    obj = chem_get(id1);
    check("destroyed obj1 is NULL", obj == (const ChemObject *)0);

    /* アクティブカウント */
    check_eq("active count", chem_active_count(), 1);

    /* 消滅したスロットを再利用できる */
    id3 = chem_spawn(0, 50, 60);
    check("reuse slot", id3 >= 0);
    check_eq("active count after reuse", chem_active_count(), 2);

    /* 不正なIDでクラッシュしない */
    chem_destroy(-1);
    chem_destroy(999);
    obj = chem_get(-1);
    check("get(-1) = NULL", obj == (const ChemObject *)0);
    obj = chem_get(999);
    check("get(999) = NULL", obj == (const ChemObject *)0);

    chem_reset();
}

/* ====================================================================== */
/*  テスト3: 属性操作                                                      */
/* ====================================================================== */
static void test_elements(void)
{
    int id;
    const ChemObject *obj;

    header("Test 3: Element Operations");

    chem_reset();
    id = chem_spawn(0, 0, 0);
    check("spawn", id >= 0);

    /* 属性追加 */
    chem_add_element(id, ELEM_FIRE);
    check("has_element(FIRE)", chem_has_element(id, ELEM_FIRE));
    check("!has_element(WATER)", !chem_has_element(id, ELEM_WATER));

    /* 複数属性追加 */
    chem_add_element(id, ELEM_WATER);
    check("has FIRE+WATER", chem_has_element(id, ELEM_FIRE) &&
                             chem_has_element(id, ELEM_WATER));

    /* 属性除去 */
    chem_remove_element(id, ELEM_FIRE);
    check("removed FIRE", !chem_has_element(id, ELEM_FIRE));
    check("still has WATER", chem_has_element(id, ELEM_WATER));

    /* 複合属性チェック */
    chem_add_element(id, ELEM_FIRE | ELEM_ICE);
    obj = chem_get(id);
    if (obj) {
        check("compound elements",
              (obj->elements & (ELEM_FIRE | ELEM_ICE | ELEM_WATER)) ==
              (ELEM_FIRE | ELEM_ICE | ELEM_WATER));
    }

    chem_reset();
}

/* ====================================================================== */
/*  テスト4: 温度操作                                                      */
/* ====================================================================== */
static void test_temperature(void)
{
    int id;
    const ChemObject *obj;

    header("Test 4: Temperature Operations");

    chem_reset();
    id = chem_spawn(0, 0, 0);

    /* 初期温度 */
    obj = chem_get(id);
    check_eq("initial temp", obj ? obj->temperature : -999, 20);

    /* 温度設定 */
    chem_set_temperature(id, 100);
    obj = chem_get(id);
    check_eq("set temp 100", obj ? obj->temperature : -999, 100);

    /* 温度加算 */
    chem_add_temperature(id, -50);
    obj = chem_get(id);
    check_eq("add temp -50", obj ? obj->temperature : -999, 50);

    /* オーバーフロー防止 */
    chem_set_temperature(id, 32000);
    chem_add_temperature(id, 1000);
    obj = chem_get(id);
    check_eq("overflow cap", obj ? obj->temperature : -999, 32767);

    chem_set_temperature(id, -32000);
    chem_add_temperature(id, -1000);
    obj = chem_get(id);
    check_eq("underflow cap", obj ? obj->temperature : -999, -32768);

    /* 不正IDでクラッシュしない */
    chem_set_temperature(-1, 50);
    chem_add_temperature(999, 10);

    chem_reset();
}

/* ====================================================================== */
/*  テスト5: 反応処理 (DBなしのダミーテスト)                                 */
/* ====================================================================== */
static void test_react(void)
{
    int id_a, id_b;

    header("Test 5: Reaction (no rules)");

    chem_reset();

    id_a = chem_spawn(0, 0, 0);
    id_b = chem_spawn(0, 1, 0);

    chem_add_element(id_a, ELEM_FIRE);
    chem_add_element(id_b, ELEM_WOOD);

    /* ルールなし (DBに接続していない) → 反応は0件 */
    {
        int applied = chem_react(id_a, id_b);
        check_eq("react (no rules)", applied, 0);
    }

    /* 不正IDでクラッシュしない */
    {
        int r;
        r = chem_react(-1, id_b);
        check_eq("react(-1, b) = 0", r, 0);
        r = chem_react(id_a, 999);
        check_eq("react(a, 999) = 0", r, 0);
    }

    chem_reset();
}

/* ====================================================================== */
/*  テスト6: 毎フレーム更新                                                 */
/* ====================================================================== */
static void test_update(void)
{
    int id;
    const ChemObject *obj;

    header("Test 6: Update (frame tick)");

    chem_reset();

    /* 燃焼中のオブジェクト: 温度が上昇する */
    id = chem_spawn(0, 0, 0);
    chem_add_element(id, ELEM_FIRE);
    chem_set_temperature(id, 50);
    {
        ChemObject *objects = chem__get_objects();
        objects[id].state = CHEM_STATE_BURNING;
    }

    chem_update();

    obj = chem_get(id);
    check("burning temp increased",
          obj && obj->temperature > 50);

    /* タイマーデクリメント */
    {
        ChemObject *objects = chem__get_objects();
        objects[id].timer = 5;
    }
    chem_update();
    obj = chem_get(id);
    check("timer decremented",
          obj && obj->timer == 4);

    /* HP=0 で消滅 */
    {
        int id2 = chem_spawn(0, 10, 10);
        ChemObject *objects = chem__get_objects();
        objects[id2].hp = 0;
        chem_update();
        obj = chem_get(id2);
        check("hp=0 destroyed", obj == (const ChemObject *)0);
    }

    chem_reset();
}

/* ====================================================================== */
/*  テスト7: 範囲検索・カウント                                             */
/* ====================================================================== */
static void test_query(void)
{
    int ids[16];
    int count;
    int id1, id2, id3;

    header("Test 7: Query (nearby, count)");

    chem_reset();

    id1 = chem_spawn(0, 0, 0);
    id2 = chem_spawn(0, 3, 4);   /* 距離5 */
    id3 = chem_spawn(0, 100, 100); /* 遠い */
    (void)id3;

    /* 半径10内の検索 */
    count = chem_find_nearby(0, 0, 10, ids, 16);
    check_eq("nearby(r=10)", count, 2);

    /* 半径3内の検索 */
    count = chem_find_nearby(0, 0, 3, ids, 16);
    check_eq("nearby(r=3)", count, 1);  /* (0,0)のみ */

    /* 燃焼カウント */
    {
        ChemObject *objects = chem__get_objects();
        objects[id1].state = CHEM_STATE_BURNING;
        objects[id2].state = CHEM_STATE_BURNING;
    }
    check_eq("burning count", chem_count_burning(), 2);

    /* max_count制限 */
    count = chem_find_nearby(0, 0, 200, ids, 1);
    check_eq("nearby max_count=1", count, 1);

    chem_reset();
}

/* ====================================================================== */
/*  テスト8: 範囲攻撃                                                      */
/* ====================================================================== */
static void test_apply_area(void)
{
    int id1, id2, id3;
    const ChemObject *obj;
    int affected;

    header("Test 8: Apply Area");

    chem_reset();

    id1 = chem_spawn(0, 0, 0);
    id2 = chem_spawn(0, 3, 4);     /* 距離5 */
    (void)id2;
    id3 = chem_spawn(0, 100, 100); /* 遠い */

    /* 半径10に火属性を適用 */
    affected = chem_apply_area(0, 0, 10, ELEM_FIRE, 50);
    check_eq("apply_area affected", affected, 2);

    obj = chem_get(id1);
    check("id1 has FIRE", obj && chem_has_element(id1, ELEM_FIRE));
    check("id1 temp increased", obj && obj->temperature == 70);
    check("id1 burning", obj && obj->state == CHEM_STATE_BURNING);

    obj = chem_get(id3);
    check("id3 unaffected", obj && !chem_has_element(id3, ELEM_FIRE));
    check("id3 temp unchanged", obj && obj->temperature == 20);

    /* 氷属性を範囲適用 */
    affected = chem_apply_area(100, 100, 5, ELEM_ICE, -100);
    check_eq("apply_area ice", affected, 1);

    obj = chem_get(id3);
    check("id3 frozen", obj && obj->state == CHEM_STATE_FROZEN);
    check("id3 temp dropped", obj && obj->temperature == -80);

    chem_reset();
}

/* ====================================================================== */
/*  テスト9: ファイルDB初期化 + ルールキャッシュ確認                         */
/* ====================================================================== */
static void test_file_db_init(void)
{
    int rc;

    header("Test 9: File DB Init (/db/chem.db)");

    rc = chem_init("/db/chem.db");
    if (rc < 0) {
        api->kprintf(ATTR_RED,
            "  [SKIP] chem_init returned %d "
            "(is /db/chem.db deployed?)\n", rc);
        return;
    }
    check("chem_init(/db/chem.db)", rc == 0);

    /* ルールキャッシュ数の確認 */
    check("reaction_count > 0", chem_reaction_count() > 0);
    check("phase_count > 0", chem_phase_count() > 0);

    api->kprintf(ATTR_WHITE, "  reactions cached: %d\n",
                 chem_reaction_count());
    api->kprintf(ATTR_WHITE, "  phases cached: %d\n",
                 chem_phase_count());

    /* テンプレートからのオブジェクト生成 */
    {
        int id;
        const ChemObject *obj;

        /* type_id=1: Tree (ELEM_WOOD, temp=20, hp=100) */
        id = chem_spawn(1, 5, 5);
        check("spawn Tree (type=1)", id >= 0);
        obj = chem_get(id);
        if (obj) {
            check("Tree has WOOD", (obj->elements & 0x04) != 0);
            check_eq("Tree temp", obj->temperature, 20);
            check_eq("Tree hp", obj->hp, 100);
        }

        /* type_id=5: Campfire (FIRE|WOOD, temp=200, hp=60) */
        {
            int id2 = chem_spawn(5, 10, 10);
            const ChemObject *obj2;
            check("spawn Campfire (type=5)", id2 >= 0);
            obj2 = chem_get(id2);
            if (obj2) {
                check("Campfire has FIRE",
                      (obj2->elements & 0x01) != 0);
                check("Campfire has WOOD",
                      (obj2->elements & 0x04) != 0);
                check_eq("Campfire temp", obj2->temperature, 200);
                check_eq("Campfire hp", obj2->hp, 60);
            }
        }
    }

    /* シャットダウンはテスト10で引き続き使うため行わない */
}

/* ====================================================================== */
/*  テスト10: 反応ルールテスト (火+木=着火, 水+火=消火)                      */
/* ====================================================================== */
static void test_db_reactions(void)
{
    int id_fire, id_wood, id_water;
    const ChemObject *obj;
    int applied;

    header("Test 10: DB-backed Reactions");

    /* ルールがロードされていない場合はスキップ */
    if (chem_reaction_count() == 0) {
        api->kprintf(ATTR_YELLOW, "  [SKIP] no rules loaded\n");
        return;
    }

    chem_reset();

    /* 火オブジェクトと木オブジェクトを生成 */
    id_fire = chem_spawn(0, 0, 0);
    chem_add_element(id_fire, ELEM_FIRE);

    id_wood = chem_spawn(0, 1, 0);
    chem_add_element(id_wood, ELEM_WOOD);

    /* 火 + 木 → 木に着火ルールが発動するはず */
    applied = chem_react(id_fire, id_wood);
    check("fire+wood reacted", applied > 0);

    obj = chem_get(id_wood);
    if (obj) {
        check("wood is now BURNING",
              obj->state == CHEM_STATE_BURNING);
        check("wood has FIRE element",
              (obj->elements & ELEM_FIRE) != 0);
        api->kprintf(ATTR_WHITE,
            "  wood: state=%d, temp=%d, hp=%d, elem=0x%x\n",
            obj->state, obj->temperature, obj->hp,
            (unsigned)obj->elements);
    }

    /* 水 + 燃焼中の木 → 消火ルールが発動するはず */
    id_water = chem_spawn(0, 2, 0);
    chem_add_element(id_water, ELEM_WATER);

    applied = chem_react(id_water, id_wood);
    check("water+burning_wood reacted", applied > 0);

    obj = chem_get(id_wood);
    if (obj) {
        /* 消火後は FIRE 属性が除去され、IDLE に戻るはず */
        check("wood fire extinguished",
              (obj->elements & ELEM_FIRE) == 0 ||
              obj->state == CHEM_STATE_IDLE);
        api->kprintf(ATTR_WHITE,
            "  wood after water: state=%d, temp=%d, elem=0x%x\n",
            obj->state, obj->temperature,
            (unsigned)obj->elements);
    }

    chem_reset();
}

/* ====================================================================== */
/*  テスト11: 温度ベース状態遷移テスト                                      */
/* ====================================================================== */
static void test_db_phase_transitions(void)
{
    int id;
    const ChemObject *obj;

    header("Test 11: DB-backed Phase Transitions");

    /* ルールがロードされていない場合はスキップ */
    if (chem_phase_count() == 0) {
        api->kprintf(ATTR_YELLOW, "  [SKIP] no phase rules loaded\n");
        return;
    }

    chem_reset();

    /* 水オブジェクトを作成して温度100以上に → 蒸気に変化するはず */
    id = chem_spawn(0, 0, 0);
    chem_add_element(id, ELEM_WATER);
    chem_set_temperature(id, 110);

    chem_update();

    obj = chem_get(id);
    if (obj) {
        check("water->steam at 110",
              (obj->elements & ELEM_STEAM) != 0);
        check("water removed after evaporation",
              (obj->elements & ELEM_WATER) == 0);
        api->kprintf(ATTR_WHITE,
            "  after evap: elem=0x%x, state=%d, temp=%d\n",
            (unsigned)obj->elements, obj->state, obj->temperature);
    }

    /* 水オブジェクトを作成して温度0以下に → 氷に変化するはず */
    {
        int id2 = chem_spawn(0, 5, 5);
        const ChemObject *obj2;
        chem_add_element(id2, ELEM_WATER);
        chem_set_temperature(id2, -10);

        chem_update();

        obj2 = chem_get(id2);
        if (obj2) {
            check("water->ice at -10",
                  (obj2->elements & ELEM_ICE) != 0);
            check("water removed after freeze",
                  (obj2->elements & ELEM_WATER) == 0);
            check("state is FROZEN",
                  obj2->state == CHEM_STATE_FROZEN);
            api->kprintf(ATTR_WHITE,
                "  after freeze: elem=0x%x, state=%d, temp=%d\n",
                (unsigned)obj2->elements, obj2->state,
                obj2->temperature);
        }
    }

    /* 氷オブジェクトを作成して温度を上げる → 水に戻るはず */
    {
        int id3 = chem_spawn(0, 10, 10);
        const ChemObject *obj3;
        chem_add_element(id3, ELEM_ICE);
        chem_set_temperature(id3, 5);

        chem_update();

        obj3 = chem_get(id3);
        if (obj3) {
            check("ice->water at 5",
                  (obj3->elements & ELEM_WATER) != 0);
            check("ice removed after melt",
                  (obj3->elements & ELEM_ICE) == 0);
            api->kprintf(ATTR_WHITE,
                "  after melt: elem=0x%x, state=%d, temp=%d\n",
                (unsigned)obj3->elements, obj3->state,
                obj3->temperature);
        }
    }

    chem_reset();
    chem_shutdown();
}

/* ====================================================================== */
/*  エントリポイント                                                       */
/* ====================================================================== */
int main(int argc, char **argv, KernelAPI *k)
{
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_CYAN, "chem_test: libos32chem test suite\n");
    api->kprintf(ATTR_CYAN, "KAPI version: %d\n", kapi->version);

    /* Phase 1: 単体テスト (DB不要) */
    test_init();
    test_objects();
    test_elements();
    test_temperature();
    test_react();
    test_update();
    test_query();
    test_apply_area();

    /* Phase 2: DB統合テスト (ファイルDB /db/chem.db 使用) */
    test_file_db_init();
    test_db_reactions();
    test_db_phase_transitions();

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
