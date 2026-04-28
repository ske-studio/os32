/* ======================================================================== */
/*  AI_TEST.C — libos32ai テストプログラム                                  */
/*                                                                          */
/*  KernelAPI kprintf でAI意思決定エンジンの全APIをテストする。              */
/*  テスト項目:                                                             */
/*    1. DB無し初期化 + 手動プロファイル設定                                 */
/*    2. ai_decide (ノイズなし/あり/ミスあり)                                */
/*    3. ai_add_noise                                                        */
/*    4. ai_weighted_pick                                                    */
/*    5. ai_lookahead_score (3種の減衰方式)                                  */
/*    6. DBありプロファイルロード                                            */
/*    7. 行動履歴 (AiHistory)                                               */
/*    8. カウンタースコア補正                                                */
/*    9. プロファイルランタイム編集                                          */
/* ======================================================================== */

#include "os32api.h"
#include "libos32ai.h"
#include "libos32math.h"
#include "libos32db.h"
#include <string.h>

extern KernelAPI *kapi;
#define api kapi

static int g_total;
static int g_passed;

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

static void check_range(const char *label, int val, int lo, int hi)
{
    g_total++;
    if (val >= lo && val <= hi) {
        g_passed++;
        api->kprintf(ATTR_GREEN, "  [OK] %s = %d [%d..%d]\n",
                     label, val, lo, hi);
    } else {
        api->kprintf(ATTR_RED, "  [NG] %s = %d, expect [%d..%d]\n",
                     label, val, lo, hi);
    }
}

static void header(const char *title)
{
    api->kprintf(ATTR_CYAN, "\n=== %s ===\n", title);
}

/* ====================================================================== */
/*  テスト1: DB無し初期化 + 手動プロファイル                                */
/* ====================================================================== */

static void test_manual_profile(void)
{
    AiProfile prof;
    int rc;

    header("Test 1: Manual Profile");

    rc = ai_init(NULL);
    check("ai_init(NULL)", rc == 0);
    check_eq("ai_profile_count", ai_profile_count(), 0);

    /* 手動プロファイル作成 */
    memset(&prof, 0, sizeof(prof));
    ai_set_param(&prof, AI_P_MISS, 0);
    ai_set_param(&prof, AI_P_NOISE, 0);

    check_eq("get_param(MISS)", (int)ai_get_param(&prof, AI_P_MISS), 0);

    ai_set_param(&prof, 15, 42);
    check_eq("set/get param[15]", (int)ai_get_param(&prof, 15), 42);

    /* 範囲外アクセス */
    check_eq("get_param(16) out-of-range", (int)ai_get_param(&prof, AI_PARAM_MAX), 0);

    ai_shutdown();
}

/* ====================================================================== */
/*  テスト2: ai_decide (決定論的テスト)                                     */
/* ====================================================================== */

static void test_decide_deterministic(void)
{
    AiProfile prof;
    AiOption opts[3];
    u8 chosen;
    int i;

    header("Test 2: Decide (Deterministic)");

    ai_init(NULL);
    memset(&prof, 0, sizeof(prof));
    /* ミス率0, ノイズ0 → 常に最高スコアを選ぶ */
    ai_set_param(&prof, AI_P_MISS, 0);
    ai_set_param(&prof, AI_P_NOISE, 0);

    opts[0].id = 10; opts[0].score = 5;
    opts[1].id = 20; opts[1].score = 15;
    opts[2].id = 30; opts[2].score = 8;

    chosen = ai_decide(&prof, opts, 3);
    check_eq("highest score (id=20)", (int)chosen, 20);

    /* 負のスコアテスト */
    opts[0].id = 1; opts[0].score = -10;
    opts[1].id = 2; opts[1].score = -5;
    opts[2].id = 3; opts[2].score = -20;

    chosen = ai_decide(&prof, opts, 3);
    check_eq("negative scores (id=2)", (int)chosen, 2);

    /* 単一選択肢 */
    opts[0].id = 99; opts[0].score = 0;
    chosen = ai_decide(&prof, opts, 1);
    check_eq("single option (id=99)", (int)chosen, 99);

    /* 空選択肢 */
    chosen = ai_decide(&prof, opts, 0);
    check_eq("count=0 -> INVALID", (int)chosen, (int)AI_INVALID_ID);

    /* ミス率100%: 毎回ランダム → 分布テスト */
    ai_set_param(&prof, AI_P_MISS, 100);
    opts[0].id = 1; opts[0].score = 100;
    opts[1].id = 2; opts[1].score = 0;
    opts[2].id = 3; opts[2].score = 0;
    {
        int counts[3];
        counts[0] = 0; counts[1] = 0; counts[2] = 0;
        rng_seed(12345);
        for (i = 0; i < 300; i++) {
            chosen = ai_decide(&prof, opts, 3);
            if (chosen == 1) counts[0]++;
            else if (chosen == 2) counts[1]++;
            else if (chosen == 3) counts[2]++;
        }

        check("miss=100, all options selected",
              counts[0] > 20 && counts[1] > 20 && counts[2] > 20);
        api->kprintf(ATTR_WHITE, "    distribution: %d/%d/%d\n",
                     counts[0], counts[1], counts[2]);
    }

    ai_shutdown();
}

/* ====================================================================== */
/*  テスト3: ai_add_noise                                                  */
/* ====================================================================== */

static void test_noise(void)
{
    AiProfile prof;
    int i;
    i16 min_seen, max_seen;
    i16 result;

    header("Test 3: Noise");

    ai_init(NULL);
    memset(&prof, 0, sizeof(prof));
    ai_set_param(&prof, AI_P_NOISE, 5);

    rng_seed(42);
    min_seen = 32767;
    max_seen = -32768;

    for (i = 0; i < 200; i++) {
        result = ai_add_noise(100, &prof);
        if (result < min_seen) min_seen = result;
        if (result > max_seen) max_seen = result;
    }

    check_range("noise range min", (int)min_seen, 95, 100);
    check_range("noise range max", (int)max_seen, 100, 105);
    api->kprintf(ATTR_WHITE, "    min=%d max=%d\n", min_seen, max_seen);

    /* ノイズ0: 値が変わらないことを確認 */
    ai_set_param(&prof, AI_P_NOISE, 0);
    result = ai_add_noise(50, &prof);
    check_eq("noise=0: unchanged", (int)result, 50);

    ai_shutdown();
}

/* ====================================================================== */
/*  テスト4: ai_weighted_pick                                              */
/* ====================================================================== */

static void test_weighted_pick(void)
{
    u16 ids[4];
    u8  weights[4];
    int counts[4];
    int i;
    u16 picked;

    header("Test 4: Weighted Pick");

    ids[0] = 100; ids[1] = 200; ids[2] = 300; ids[3] = 400;
    weights[0] = 1; weights[1] = 3; weights[2] = 1; weights[3] = 5;
    counts[0] = 0; counts[1] = 0; counts[2] = 0; counts[3] = 0;

    rng_seed(9999);

    for (i = 0; i < 1000; i++) {
        picked = ai_weighted_pick(ids, weights, 4);
        if (picked == 100) counts[0]++;
        else if (picked == 200) counts[1]++;
        else if (picked == 300) counts[2]++;
        else if (picked == 400) counts[3]++;
    }

    /* 重み比 1:3:1:5 → 400が最も多い */
    check("400 most frequent", counts[3] > counts[1] && counts[1] > counts[0]);
    api->kprintf(ATTR_WHITE, "    100=%d 200=%d 300=%d 400=%d\n",
                 counts[0], counts[1], counts[2], counts[3]);

    /* 全重み0: フォールバック */
    {
        u8 zero_w[4];
        zero_w[0] = 0; zero_w[1] = 0; zero_w[2] = 0; zero_w[3] = 0;
        picked = ai_weighted_pick(ids, zero_w, 4);
        check_eq("all-zero weights -> ids[0]", (int)picked, 100);
    }

    /* 単一候補 */
    picked = ai_weighted_pick(ids, weights, 1);
    check_eq("single candidate", (int)picked, 100);
}

/* ====================================================================== */
/*  テスト5: ai_lookahead_score                                            */
/* ====================================================================== */

static void test_lookahead(void)
{
    i16 scores[4];
    i16 result;

    header("Test 5: Lookahead Score");

    scores[0] = 10; scores[1] = 8; scores[2] = 6; scores[3] = 4;

    /* 均等: 10+8+6+4 = 28 */
    result = ai_lookahead_score(scores, 4, AI_DECAY_EQUAL);
    check_eq("EQUAL: 10+8+6+4", (int)result, 28);

    /* 線形: 10*4 + 8*3 + 6*2 + 4*1 = 80 */
    result = ai_lookahead_score(scores, 4, AI_DECAY_LINEAR);
    check_eq("LINEAR: 10*4+8*3+6*2+4*1", (int)result, 80);

    /* 指数: 10 + 8/2 + 6/4 + 4/8 = 10+4+1+0 = 15 */
    result = ai_lookahead_score(scores, 4, AI_DECAY_EXP);
    check_eq("EXP: 10+4+1+0", (int)result, 15);

    /* 空 */
    result = ai_lookahead_score(scores, 0, AI_DECAY_EQUAL);
    check_eq("count=0 -> 0", (int)result, 0);
}

/* ====================================================================== */
/*  テスト6: DBプロファイルロード                                           */
/* ====================================================================== */

static void test_db_profile(void)
{
    AiProfile prof;
    int rc;

    header("Test 6: DB Profile Load");

    rc = ai_init("/host/assets/ai.db");
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW,
            "  [SKIP] ai_init returned %d "
            "(is ai.db deployed?)\n", rc);
        return;
    }
    check("ai_init(/host/assets/ai.db)", rc == 0);

    api->kprintf(ATTR_WHITE, "  profile_count = %d\n", ai_profile_count());
    check("profile_count > 0", ai_profile_count() > 0);

    rc = ai_load_profile(0, &prof);
    check("load profile 0", rc == 0);
    if (rc == 0) {
        api->kprintf(ATTR_WHITE, "  prof[0]: miss=%d, noise=%d, p2=%d\n",
                     ai_get_param(&prof, AI_P_MISS),
                     ai_get_param(&prof, AI_P_NOISE),
                     ai_get_param(&prof, 2));
    }

    /* DBプロファイルでのai_decide */
    {
        AiOption opts[3];
        u8 chosen;
        opts[0].id = 1; opts[0].score = 10;
        opts[1].id = 2; opts[1].score = 20;
        opts[2].id = 3; opts[2].score = 15;

        rng_seed(777);
        chosen = ai_decide(&prof, opts, 3);
        api->kprintf(ATTR_WHITE, "  decide with DB profile: chosen=%d\n",
                     (int)chosen);
        check("decide result valid", chosen >= 1 && chosen <= 3);
    }

    ai_shutdown();
}

/* ====================================================================== */
/*  テスト7: 行動履歴 (AiHistory)                                          */
/* ====================================================================== */

static void test_history(void)
{
    AiHistory hist;
    int i;

    header("Test 7: History");

    /* リセット確認 */
    ai_history_reset(&hist);
    check_eq("reset: total", (int)hist.total, 0);
    check_eq("reset: counts[0]", (int)hist.action_counts[0], 0);

    /* 行動記録 */
    ai_history_record(&hist, 0);
    ai_history_record(&hist, 0);
    ai_history_record(&hist, 0);
    ai_history_record(&hist, 1);
    ai_history_record(&hist, 2);

    check_eq("record: total", (int)hist.total, 5);
    check_eq("record: counts[0]", (int)hist.action_counts[0], 3);
    check_eq("record: counts[1]", (int)hist.action_counts[1], 1);
    check_eq("record: counts[2]", (int)hist.action_counts[2], 1);

    /* 最頻行動 */
    check_eq("most_common = 0", (int)ai_history_most_common(&hist), 0);

    /* 選択率 */
    check_eq("ratio(0) = 60%", (int)ai_history_ratio(&hist, 0), 60);
    check_eq("ratio(1) = 20%", (int)ai_history_ratio(&hist, 1), 20);

    /* 空の履歴 */
    {
        AiHistory empty;
        ai_history_reset(&empty);
        check_eq("empty: most_common = 0",
                 (int)ai_history_most_common(&empty), 0);
        check_eq("empty: ratio = 0", (int)ai_history_ratio(&empty, 0), 0);
    }

    /* 飽和防止テスト: 大量記録して半減メカニズムを検証 */
    ai_history_reset(&hist);
    for (i = 0; i < 260; i++) {
        ai_history_record(&hist, 0);
    }
    /* 254到達時に半減が発生し、255を超えないことを確認 */
    check("saturation: total <= 255", (int)hist.total <= 255);
    check("saturation: counts[0] > 0", (int)hist.action_counts[0] > 0);
    api->kprintf(ATTR_WHITE, "    after 260 records: total=%d, counts[0]=%d\n",
                 (int)hist.total, (int)hist.action_counts[0]);

    /* 範囲外行動ID: 無視されることを確認 */
    {
        u8 prev_total;
        prev_total = hist.total;
        ai_history_record(&hist, AI_HISTORY_ACTIONS);   /* 範囲外 */
        check_eq("out-of-range ignored", (int)hist.total, (int)prev_total);
    }
}

/* ====================================================================== */
/*  テスト8: カウンタースコア補正                                           */
/* ====================================================================== */

static void test_counter_score(void)
{
    AiHistory hist;
    i16 score;

    header("Test 8: Counter Score");

    /* 履歴セットアップ: action 0 が 80%, action 1 が 20% */
    ai_history_reset(&hist);
    {
        int i;
        for (i = 0; i < 80; i++) ai_history_record(&hist, 0);
        for (i = 0; i < 20; i++) ai_history_record(&hist, 1);
    }

    check_eq("setup: ratio(0)", (int)ai_history_ratio(&hist, 0), 80);
    check_eq("setup: ratio(1)", (int)ai_history_ratio(&hist, 1), 20);

    /* PUNISH: 頻出行動のスコアを下げる */
    /* action 0: ratio=80, strength=10 → delta = 80*10/100 = 8 */
    score = ai_counter_score(100, 0, &hist, 10, AI_COUNTER_PUNISH);
    check_eq("PUNISH action0: 100-8", (int)score, 92);

    /* action 1: ratio=20, strength=10 → delta = 20*10/100 = 2 */
    score = ai_counter_score(100, 1, &hist, 10, AI_COUNTER_PUNISH);
    check_eq("PUNISH action1: 100-2", (int)score, 98);

    /* EXPLOIT: 頻出行動のスコアを上げる */
    score = ai_counter_score(50, 0, &hist, 10, AI_COUNTER_EXPLOIT);
    check_eq("EXPLOIT action0: 50+8", (int)score, 58);

    score = ai_counter_score(50, 1, &hist, 10, AI_COUNTER_EXPLOIT);
    check_eq("EXPLOIT action1: 50+2", (int)score, 52);

    /* strength=0: 補正なし */
    score = ai_counter_score(100, 0, &hist, 0, AI_COUNTER_PUNISH);
    check_eq("strength=0: no change", (int)score, 100);

    /* 空の履歴: 補正なし */
    {
        AiHistory empty;
        ai_history_reset(&empty);
        score = ai_counter_score(100, 0, &empty, 10, AI_COUNTER_PUNISH);
        check_eq("empty hist: no change", (int)score, 100);
    }

    /* 高strength */
    score = ai_counter_score(100, 0, &hist, 50, AI_COUNTER_PUNISH);
    /* delta = 80*50/100 = 40 → 100-40 = 60 */
    check_eq("PUNISH high str: 100-40", (int)score, 60);

    /* カウンター補正付きdecide統合テスト */
    {
        AiProfile prof;
        AiOption opts[3];
        u8 chosen;
        int punish_wins, raw_wins, j;

        ai_init(NULL);
        memset(&prof, 0, sizeof(prof));
        ai_set_param(&prof, AI_P_MISS, 0);
        ai_set_param(&prof, AI_P_NOISE, 0);

        /* action 0 が圧倒的に多い履歴に対して */
        /* PUNISH モードで補正するとaction 0が選ばれにくくなる */
        punish_wins = 0;
        raw_wins = 0;
        rng_seed(42);
        for (j = 0; j < 100; j++) {
            /* 補正なし: action 0 が常に勝つ */
            opts[0].id = 0; opts[0].score = 50;
            opts[1].id = 1; opts[1].score = 45;
            opts[2].id = 2; opts[2].score = 40;

            chosen = ai_decide(&prof, opts, 3);
            if (chosen == 0) raw_wins++;

            /* PUNISH補正付き */
            opts[0].score = ai_counter_score(50, 0, &hist, 15, AI_COUNTER_PUNISH);
            opts[1].score = ai_counter_score(45, 1, &hist, 15, AI_COUNTER_PUNISH);
            opts[2].score = ai_counter_score(40, 2, &hist, 15, AI_COUNTER_PUNISH);

            chosen = ai_decide(&prof, opts, 3);
            if (chosen == 0) punish_wins++;
        }

        api->kprintf(ATTR_WHITE,
            "    raw: action0 wins=%d, punish: action0 wins=%d\n",
            raw_wins, punish_wins);

        /* 補正ありではaction0の勝率が下がることを確認 */
        check("punish reduces action0 wins", punish_wins < raw_wins);

        ai_shutdown();
    }
}

/* ====================================================================== */
/*  テスト9: プロファイルランタイム編集                                      */
/* ====================================================================== */

static void test_runtime_profile(void)
{
    AiProfile prof;
    AiProfile loaded;
    int rc;

    header("Test 9: Runtime Profile Edit");

    ai_init(NULL);

    /* 手動プロファイルをキャッシュに登録 */
    memset(&prof, 0, sizeof(prof));
    ai_set_param(&prof, AI_P_MISS, 25);
    ai_set_param(&prof, AI_P_NOISE, 7);
    ai_set_param(&prof, 2, 80);
    prof.param_count = 3;

    rc = ai_update_profile(0, &prof);
    check_eq("update_profile(0)", rc, 0);
    check_eq("profile_count after update", ai_profile_count(), 1);

    /* ロードして一致確認 */
    memset(&loaded, 0, sizeof(loaded));
    rc = ai_load_profile(0, &loaded);
    check("load after update", rc == 0);
    check_eq("loaded MISS", (int)ai_get_param(&loaded, AI_P_MISS), 25);
    check_eq("loaded NOISE", (int)ai_get_param(&loaded, AI_P_NOISE), 7);
    check_eq("loaded p2", (int)ai_get_param(&loaded, 2), 80);

    /* パラメータ変更して再登録 */
    ai_set_param(&prof, AI_P_MISS, 50);
    rc = ai_update_profile(0, &prof);
    check_eq("re-update_profile", rc, 0);

    rc = ai_load_profile(0, &loaded);
    check_eq("re-loaded MISS", (int)ai_get_param(&loaded, AI_P_MISS), 50);

    /* 範囲外テスト */
    rc = ai_update_profile(AI_MAX_PROFILES, &prof);
    check_eq("update out-of-range", rc, -1);

    /* DB未接続時のsave */
    rc = ai_save_profile(0);
    check_eq("save without DB", rc, -1);

    ai_shutdown();
}

/* ====================================================================== */
/*  テスト10: DB永続化テスト                                                */
/* ====================================================================== */

static void test_db_save(void)
{
    AiProfile prof;
    AiProfile loaded;
    int rc;

    header("Test 10: DB Save Profile");

    rc = ai_init("/host/assets/ai.db");
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW,
            "  [SKIP] ai_init returned %d\n", rc);
        return;
    }
    check("ai_init", rc == 0);

    /* 既存プロファイルをロード → 変更 → 保存 → 再ロードで確認 */
    rc = ai_load_profile(0, &prof);
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW, "  [SKIP] no profile 0\n");
        ai_shutdown();
        return;
    }

    /* ノイズ値を変更して保存 */
    {
        u8 orig_noise;
        u8 new_noise;

        orig_noise = ai_get_param(&prof, AI_P_NOISE);
        new_noise = (orig_noise == 99) ? 1 : (u8)(orig_noise + 1);
        ai_set_param(&prof, AI_P_NOISE, new_noise);

        rc = ai_update_profile(0, &prof);
        check_eq("update for save", rc, 0);

        rc = ai_save_profile(0);
        check_eq("save_profile(0)", rc, 0);
        api->kprintf(ATTR_WHITE, "    noise: %d -> %d\n",
                     (int)orig_noise, (int)new_noise);

        /* キャッシュから再ロードして確認 */
        rc = ai_load_profile(0, &loaded);
        check_eq("reload noise", (int)ai_get_param(&loaded, AI_P_NOISE),
                 (int)new_noise);

        /* 元に戻す */
        ai_set_param(&prof, AI_P_NOISE, orig_noise);
        ai_update_profile(0, &prof);
        ai_save_profile(0);
    }

    ai_shutdown();
}

/* ====================================================================== */
/*  エントリポイント                                                       */
/* ====================================================================== */

int main(int argc, char **argv, KernelAPI *k)
{
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_CYAN, "ai_test: libos32ai test suite (Phase 1+2)\n");
    api->kprintf(ATTR_CYAN, "KAPI version: %d\n", kapi->version);

    /* 乱数初期化 */
    rng_seed(0xDEADBEEF);

    /* Phase 1 テスト */
    test_manual_profile();
    test_decide_deterministic();
    test_noise();
    test_weighted_pick();
    test_lookahead();
    test_db_profile();

    /* Phase 2 テスト */
    test_history();
    test_counter_score();
    test_runtime_profile();
    test_db_save();

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
