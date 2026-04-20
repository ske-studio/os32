/* ======================================================================== */
/*  FEP_TEST.C - FEP 辞書検索エンジン テストアプリ                           */
/*                                                                          */
/*  ハードコードされたひらがなクエリで辞書検索を行い、結果を出力する。        */
/*  get_tick() で検索時間を計測する。                                        */
/* ======================================================================== */

#include "os32api.h"
#include "fep_engine.h"

/* テストクエリ (UTF-8 ひらがな) */
static const char *test_queries[] = {
    "\xe3\x81\x8b\xe3\x82\x93\xe3\x81\x98",             /* かんじ */
    "\xe3\x81\x8d\xe3\x82\x87\xe3\x81\x86",             /* きょう */
    "\xe3\x81\x97\xe3\x82\x87",                         /* しょ (前方一致テスト: 最大ブロック) */
    "\xe3\x81\x82",                                     /* あ (1文字テスト) */
    "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xb4\xe3\x82\x85" /* こんぴゅ (前方一致) */
    "\xe3\x83\xbc\xe3\x81\x9f",                         /* ーた */
    "\xe3\x81\x9f\xe3\x81\xae\xe3\x81\x97\xe3\x81\x84", /* たのしい */
    NULL
};

/* テストクエリの表示用名前 (ASCII) */
static const char *test_names[] = {
    "kanji",
    "kyou",
    "sho (prefix)",
    "a (1-char)",
    "konpyu-ta",
    "tanoshii",
    NULL
};

/* 表示する最大候補数 */
#define MAX_DISPLAY 10

void main(int argc, char **argv, KernelAPI *api)
{
    FEP_Dict dict;
    FEP_Result results[FEP_MAX_RESULTS];
    int ret, i, q;
    u32 t_start, t_end;

    api->kprintf(ATTR_CYAN, "=== FEP Dictionary Search Test ===\r\n");
    api->kprintf(ATTR_WHITE, "Loading dictionary: fep.dic ...\r\n");

    ret = fep_dict_open(&dict, "fep.dic", api);
    if (ret != 0) {
        api->kprintf(ATTR_RED, "Failed to open dictionary (err=%d)\r\n", ret);
        return;
    }

    api->kprintf(ATTR_GREEN, "Dict loaded: %d words, L1=%d, L2=%d\r\n",
                 dict.header.total_words,
                 dict.header.l1_count,
                 dict.l2_total);
    api->kprintf(ATTR_WHITE, "\r\n");

    /* テストクエリを順次実行 */
    for (q = 0; test_queries[q] != NULL; q++) {
        int count;
        int display_count;

        api->kprintf(ATTR_YELLOW, "--- Test %d: \"%s\" ---\r\n", q + 1, test_names[q]);

        t_start = api->get_tick();
        count = fep_dict_search(&dict, test_queries[q], results, FEP_MAX_RESULTS);
        t_end = api->get_tick();

        if (count < 0) {
            api->kprintf(ATTR_RED, "Search error: %d\r\n", count);
            continue;
        }

        if (count == 0) {
            api->kprintf(ATTR_WHITE, "No results.\r\n");
        } else {
            display_count = count < MAX_DISPLAY ? count : MAX_DISPLAY;
            for (i = 0; i < display_count; i++) {
                api->kprintf(ATTR_WHITE, "[%d] %s (cost=%d, pos=%d)\r\n",
                             i + 1,
                             results[i].kanji,
                             results[i].cost,
                             results[i].pos_id);
            }
            if (count > MAX_DISPLAY) {
                api->kprintf(ATTR_WHITE, "  ... +%d more\r\n", count - MAX_DISPLAY);
            }
        }

        api->kprintf(ATTR_CYAN, "Time: %d ms, Results: %d\r\n\r\n",
                     t_end - t_start, count);
    }

    fep_dict_close(&dict);
    api->kprintf(ATTR_GREEN, "FEP test complete.\r\n");
}
