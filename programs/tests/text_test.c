/* ======================================================================== */
/*  TEXT_TEST.C — libos32text 単体テスト                                    */
/*                                                                          */
/*  GFX不要。シリアル出力で状態遷移を確認する。                              */
/*  テスト用DB: /db/text.db (text_db_init.py で生成)                        */
/* ======================================================================== */

#include "os32api.h"
#include "libos32text.h"

extern KernelAPI *kapi;
#define api kapi

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

/* テスト用完了コールバック */
static int cb_slot = -1;
static u16 cb_msg_id = 0;
static int cb_count = 0;

static void on_done(int slot, u16 msg_id)
{
    cb_slot = slot;
    cb_msg_id = msg_id;
    cb_count++;
}

/* ====================================================================== */
/*  テスト                                                                  */
/* ====================================================================== */

/* Test 1: 初期化・終了 */
static void test_init_shutdown(void)
{
    int rc;
    header("Test 1: init/shutdown");

    rc = text_init("/db/text.db");
    check("text_init succeeded", rc == TEXT_OK);
    check_eq("no active slots", text__slot_active_count(), 0);

    text_shutdown();
    check_eq("shutdown clears slots", text__slot_active_count(), 0);
}

/* Test 2: 単一メッセージ取得 (ASCII) */
static void test_load_ascii(void)
{
    int rc;
    const char *vis;
    int len;

    header("Test 2: load ASCII message");
    rc = text_init("/db/text.db");
    check("init ok", rc == TEXT_OK);

    rc = text_load(0, 1);
    check("text_load(0, 1) ok", rc == TEXT_OK);
    check_eq("state=TYPING", text_get_state(0), TEXT_STATE_TYPING);
    check("slot 0 active", text_is_active(0) == 1);
    check_eq("msg_id=1", text_get_msg_id(0), 1);

    /* visible_len は最初 0 */
    vis = text_get_visible(0, &len);
    check("visible not NULL", vis != 0);
    check_eq("initial visible_len=0", len, 0);

    text_shutdown();
}

/* Test 3: タイプライター進行 */
static void test_typewriter(void)
{
    int rc, i;
    int len;
    const char *vis;

    header("Test 3: typewriter progression");
    rc = text_init("/db/text.db");
    (void)rc;

    /* msg_id=1: "Hello, World!" (13文字, speed=2) */
    text_load(0, 1);

    /* 2フレーム進行 → 1文字目表示 */
    text_update();
    text_update();
    vis = text_get_visible(0, &len);
    check_eq("after 2 frames: 1 char visible", len, 1);
    check("first char is 'H'", vis && vis[0] == 'H');

    /* さらに進行して全文表示 */
    for (i = 0; i < 100; i++)
        text_update();

    check_eq("state=DONE after full progression",
             text_get_state(0), TEXT_STATE_DONE);

    vis = text_get_visible(0, &len);
    check_eq("full text visible (13 bytes)", len, 13);

    text_shutdown();
}

/* Test 4: 即時全文表示 (skip) */
static void test_skip(void)
{
    int len;
    header("Test 4: skip (instant display)");
    text_init("/db/text.db");
    text_load(0, 1);

    check_eq("state=TYPING before skip",
             text_get_state(0), TEXT_STATE_TYPING);

    text_skip(0);

    check_eq("state=DONE after skip",
             text_get_state(0), TEXT_STATE_DONE);
    text_get_visible(0, &len);
    check_eq("full text after skip", len, 13);

    text_shutdown();
}

/* Test 5: ページ分割 (\p) */
static void test_pages(void)
{
    int rc;
    header("Test 5: page splitting (\\p)");
    text_init("/db/text.db");

    /* msg_id=4: 3ページ */
    rc = text_load(0, 4);
    check("load multi-page message", rc == TEXT_OK);
    check_eq("3 pages", text_get_page_count(0), 3);
    check_eq("current_page=0", text_get_page(0), 0);

    /* 1ページ目をスキップ */
    text_skip(0);
    check_eq("state=WAIT after skip page 1",
             text_get_state(0), TEXT_STATE_WAIT);
    check_eq("still on page 0", text_get_page(0), 0);

    text_shutdown();
}

/* Test 6: ページ送り (advance) */
static void test_advance(void)
{
    int rc;
    header("Test 6: page advance");
    text_init("/db/text.db");

    text_load(0, 4); /* 3ページ */
    text_skip(0);     /* 1ページ目完了 → WAIT */

    rc = text_advance(0);
    check_eq("advance to page 2", rc, TEXT_OK);
    check_eq("current_page=1", text_get_page(0), 1);
    check_eq("state=TYPING on page 2",
             text_get_state(0), TEXT_STATE_TYPING);

    text_skip(0);     /* 2ページ目完了 → WAIT */
    rc = text_advance(0);
    check_eq("advance to page 3", rc, TEXT_OK);

    text_skip(0);     /* 3ページ目完了 → DONE */
    check_eq("state=DONE on last page",
             text_get_state(0), TEXT_STATE_DONE);

    rc = text_advance(0);
    check_eq("advance returns END on last page", rc, TEXT_ERR_END);

    text_shutdown();
}

/* Test 7: 変数展開 */
static void test_variables(void)
{
    int len;
    const char *vis;

    header("Test 7: variable expansion");
    text_init("/db/text.db");

    text_set_var(0, "Taro");
    text_set_var(1, "Sword");

    /* msg_id=3: "{0}は {1} を手に入れた！" */
    text_load(0, 3);
    text_skip(0);

    vis = text_get_visible(0, &len);
    check("visible not NULL", vis != 0);
    check("visible_len > 0", len > 0);

    /* "Taro" が含まれているか簡易チェック */
    {
        int found = 0;
        int i;
        for (i = 0; i < len - 3; i++) {
            if (vis[i] == 'T' && vis[i+1] == 'a' && vis[i+2] == 'r' && vis[i+3] == 'o') {
                found = 1;
                break;
            }
        }
        check("expanded text contains 'Taro'", found);
    }

    text_clear_vars();
    text_shutdown();
}

/* Test 8: グループ連続取得 */
static void test_group(void)
{
    int rc;
    header("Test 8: group sequential load");
    text_init("/db/text.db");

    /* グループ1: オープニング (3メッセージ) */
    rc = text_load_group(0, 1);
    check("load_group(1) ok", rc == TEXT_OK);
    check_eq("first msg_id=10", text_get_msg_id(0), 10);

    text_skip(0);
    /* DONEからnext_messageに進むため、advanceでDONEを消化する必要なし */
    rc = text_next_message(0);
    check("next_message ok (seq=1)", rc == TEXT_OK);
    check_eq("second msg_id=11", text_get_msg_id(0), 11);

    text_skip(0);
    rc = text_next_message(0);
    check("next_message ok (seq=2)", rc == TEXT_OK);
    check_eq("third msg_id=12", text_get_msg_id(0), 12);

    text_skip(0);
    rc = text_next_message(0);
    check_eq("next_message returns END at group end", rc, TEXT_ERR_END);

    text_shutdown();
}

/* Test 9: エラー処理 (存在しないメッセージ) */
static void test_error_notfound(void)
{
    int rc;
    header("Test 9: error: message not found");
    text_init("/db/text.db");

    rc = text_load(0, 9999);
    check_eq("load non-existent msg returns NOTFOUND", rc, TEXT_ERR_NOTFOUND);
    check("slot not active after error", text_is_active(0) == 0);

    text_shutdown();
}

/* Test 10: エラー処理 (スロット範囲外) */
static void test_error_slot(void)
{
    int rc;
    header("Test 10: error: invalid slot");
    text_init("/db/text.db");

    rc = text_load(-1, 1);
    check_eq("negative slot returns ERR_SLOT", rc, TEXT_ERR_SLOT);

    rc = text_load(TEXT_MAX_SLOTS, 1);
    check_eq("out-of-range slot returns ERR_SLOT", rc, TEXT_ERR_SLOT);

    text_shutdown();
}

/* Test 11: 話者名 */
static void test_speaker(void)
{
    const char *sp;
    header("Test 11: speaker name");
    text_init("/db/text.db");

    /* msg_id=6: speaker="村人A" */
    text_load(0, 6);
    sp = text_get_speaker(0);
    check("speaker not NULL", sp != 0);
    check("speaker not empty", sp && sp[0] != '\0');

    /* msg_id=1: speaker=NULL */
    text_load(1, 1);
    sp = text_get_speaker(1);
    check("narration speaker not NULL", sp != 0);
    check("narration speaker is empty string", sp && sp[0] == '\0');

    text_shutdown();
}

/* Test 12: 完了コールバック */
static void test_callback(void)
{
    header("Test 12: done callback");
    text_init("/db/text.db");

    cb_count = 0;
    cb_slot = -1;
    cb_msg_id = 0;
    text_set_done_callback(on_done);

    text_load(0, 1);
    text_skip(0);

    check_eq("callback fired once", cb_count, 1);
    check_eq("callback slot=0", cb_slot, 0);
    check_eq("callback msg_id=1", cb_msg_id, 1);

    text_shutdown();
}

/* ====================================================================== */
/*  メイン                                                                  */
/* ====================================================================== */

int main(int argc, char *argv[], KernelAPI *k)
{
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_CYAN, "========================================\n");
    api->kprintf(ATTR_CYAN, "  libos32text Test Suite\n");
    api->kprintf(ATTR_CYAN, "========================================\n");
    api->kprintf(ATTR_WHITE, "  KAPI version: %d\n", kapi->version);

    test_init_shutdown();
    test_load_ascii();
    test_typewriter();
    test_skip();
    test_pages();
    test_advance();
    test_variables();
    test_group();
    test_error_notfound();
    test_error_slot();
    test_speaker();
    test_callback();

    api->kprintf(ATTR_CYAN, "\n========================================\n");
    api->kprintf(ATTR_CYAN, "  Results: %d/%d passed",
                 g_passed, g_total);
    if (g_total - g_passed > 0)
        api->kprintf(ATTR_RED, " (%d FAILED)", g_total - g_passed);
    api->kprintf(ATTR_WHITE, "\n");
    api->kprintf(ATTR_CYAN, "========================================\n");

    return (g_total - g_passed > 0) ? 1 : 0;
}
