/* ======================================================================== */
/*  VIEW_EXPORT.C — 自動プレイ観測用の状態メールボックス                     */
/*                                                                          */
/*  レイアウトはホスト側 tools/autoplay/driver.py の parse_mailbox() と      */
/*  一対で、変えるときは必ず両方 + EXPORT_VERSION を更新すること。           */
/*                                                                          */
/*  torn read 対策: 書き込み開始時に seq_open を進め、本体を書き終えてから   */
/*  seq_close に同じ値を書く。ホストは両者が一致するまで読み直す。           */
/* ======================================================================== */

#include "view_export.h"
#include "view_panel.h"
#include "game_glue.h"
#include "libos32db.h"
#include <string.h>

/* 全フィールドを自然整列で並べ、パディングを明示して 832 バイト固定にする */
typedef struct {
    u32 magic;              /* 0   'GST1' */
    u32 seq_open;           /* 4   */
    u16 version;            /* 8   */
    u16 total_size;         /* 10  */
    u32 frame;              /* 12  毎フレーム+1 (生存確認) */
    i32 game_state;         /* 16  ST_* */
    u16 turn;               /* 20  */
    u8  week;               /* 22  */
    u8  cur_player;         /* 23  */
    u8  num_players;        /* 24  */
    u8  boss_progress;      /* 25  1..8, 9=全滅済 */
    u8  dungeon_floor;      /* 26  0=潜行していない */
    u8  victory_type;       /* 27  GLUE_WIN_* */
    u32 dungeon_loot;       /* 28  */
    i8  victory_winner;     /* 32  -1=未決着 */
    i8  dice_value;         /* 33  */
    u16 _pad;               /* 34  */
    struct {
        char name[32];      /* +0  UTF-8 */
        i16  hp;            /* +32 */
        i16  max_hp;        /* +34 */
        u16  pos;           /* +36 */
        u8   level;         /* +38 */
        u8   flags;         /* +39 bit0=cpu, bit1=devil */
        u32  gold;          /* +40 */
        u8   equip_count;   /* +44 装備スロット数 (変身で壊れないかの監視) */
        u8   _p;            /* +45 */
        i16  eff_atk;       /* +46 装備込みの攻撃力 */
        i16  eff_def;       /* +48 装備込みの防御力 */
        u16  _p2;           /* +50 */
    } players[4];           /* 36..243 (52バイト x 4) */
    u8  line_count;         /* 244 */
    u8  attrs[5];           /* 245..249 */
    u16 _pad2;              /* 250 */
    char lines[5][121];     /* 252..856 UTF-8 */
    u8  _pad3[3];           /* 857..859 */
    u32 db_mem;             /* 860  SQLite MEMSYS5 使用量 (リーク検出用) */
    u32 seq_close;          /* 864 */
} GameStateExport;          /* = 868 バイト */

/* レイアウトが崩れたらコンパイルエラーにする (C89 の static assert) */
typedef char export_size_check[(sizeof(GameStateExport) == 868) ? 1 : -1];

void view_export_tick(int game_state, int dice_value)
{
    volatile GameStateExport *ex =
        (volatile GameStateExport *)EXPORT_MAILBOX_ADDR;
    static u32 seq = 0;
    static u32 frame = 0;
    int i;

    seq++;
    frame++;

    ex->seq_open = seq;
    ex->magic = EXPORT_MAGIC;
    ex->version = EXPORT_VERSION;
    ex->total_size = (u16)sizeof(GameStateExport);
    ex->frame = frame;
    ex->game_state = (i32)game_state;
    ex->turn = glue_turn;
    ex->week = (u8)glue_week;
    ex->cur_player = glue_current_player;
    ex->num_players = glue_num_players;
    ex->boss_progress = glue_boss_progress;
    ex->dungeon_floor = (u8)glue_dungeon_floor;
    ex->victory_type = (u8)glue_victory_type;
    ex->dungeon_loot = glue_dungeon_loot;
    ex->victory_winner = (i8)glue_victory_winner;
    ex->dice_value = (i8)dice_value;
    ex->_pad = 0;

    for (i = 0; i < 4; i++) {
        const GluePlayer *p = &glue_players[i];
        memcpy((void *)ex->players[i].name, p->name, 32);
        ex->players[i].hp = p->actor.hp;
        ex->players[i].max_hp = p->actor.max_hp;
        ex->players[i].pos = p->pos;
        ex->players[i].level = (u8)p->actor.level;
        ex->players[i].flags = (u8)((p->is_cpu ? 1 : 0) |
                                    (p->is_devil ? 2 : 0));
        ex->players[i].gold = p->gold;
        ex->players[i].equip_count = p->bag.equip_count;
        ex->players[i].eff_atk = (i16)glue_player_effective_atk(i);
        ex->players[i].eff_def = (i16)glue_player_effective_def(i);
        ex->players[i]._p = 0;
        ex->players[i]._p2 = 0;
    }

    ex->line_count = (u8)panel_get_count();
    for (i = 0; i < 5; i++) {
        const char *ln = (i < panel_get_count()) ? panel_get_line(i) : "";
        ex->attrs[i] = (i < panel_get_count()) ? panel_get_attr(i) : 0;
        strncpy((char *)ex->lines[i], ln, 120);
        ex->lines[i][120] = '\0';
    }
    ex->_pad2 = 0;
    ex->_pad3[0] = ex->_pad3[1] = ex->_pad3[2] = 0;
    /* ゲーム再起動などで SQLite プールが漏れていないかをホストから見る */
    ex->db_mem = db_mem_used();

    ex->seq_close = seq;
}
