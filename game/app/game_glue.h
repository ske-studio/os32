/* ======================================================================== */
/*  GAME_GLUE.H — ゲームエンジン連携（グルー）インターフェース                */
/* ======================================================================== */

#ifndef GAME_GLUE_H
#define GAME_GLUE_H

#include "os32api.h"
#include "libos32battle.h"
#include "libos32econ.h"
#include "libos32inv.h"
#include "libos32ai.h"
#include "libos32board.h"
#include "libos32rpg.h"
#include "libos32turn.h"
#include "libos32event.h"

/* 定数定義 */
#define GLUE_MAX_PLAYERS 4
#define GLUE_MAX_ITEMS 10
/* 装備スロット数 (武器/盾/鎧/アクセサリ)。items.db のアクセサリは
   equip_slot=3 に揃えてある。inv_bag_init を呼ぶ箇所は必ずこれを使うこと
   — デビル変身時に 3 を直書きしていて、変身するとアクセサリ枠が
   永久に消えるバグがあった (2026-08-18 修正) */
#define GLUE_EQUIP_SLOTS 4

/* デビル (イザナミ) 専用装備の items.db 上の ID。
   変身中だけ強制装備し、解除で取り上げる。店・抽選には出さない。
   以前は glue_player_effective_atk/def に +86 / +105 を直書きしていたが、
   装備として持たせることで装備ボーナスの計算経路を一本化した */
#define GLUE_DEVIL_WEAPON 90   /* 天沼矛   ATK+86 */
#define GLUE_DEVIL_SHIELD 91   /* 黄泉の盾 DEF+43 */
#define GLUE_DEVIL_ARMOR  92   /* 黄泉の鎧 DEF+62 */
#define GLUE_MAX_FMAGIC 8
#define GLUE_MAX_ENEMIES 64   /* battle.db の enemies は 52 件 */
#define GLUE_ENEMY_NAME_LEN 24

/* 敵マスタ 1 件 (battle.db enemies テーブル)。
   libos32battle は敵マスタの概念を持たない (BtlUnit は常に呼び出し側が
   用意する) ため、これはゲーム側の所有物として game_glue が直接読む。 */
typedef struct {
    u16 id;
    u8  stage;                          /* 出現ステージ 1-8 (board の area) */
    u8  kind;                           /* 0=野生, 1=ボス */
    char name[GLUE_ENEMY_NAME_LEN];
    i16 max_hp;
    i16 atk;
    i16 def;
    i16 spd;
    i16 mag;
    u32 elements;
    u32 exp;                            /* 撃破時の獲得経験値 */
    u32 gold;                           /* 撃破時の獲得金額 */
    u8  class_id;
    u8  _pad[3];
} GlueEnemy;

/* プレイヤー構造体 (C89)
   レベル/経験値/HP/基礎パラメータ/状態異常は libos32rpg の RpgActor が
   単一の情報源。GluePlayer 側に複製は持たない (二重管理を避ける)。 */
typedef struct {
    char name[32];
    u8 ujigami;        /* 氏神ID。RpgActor.class_id と同値 (成長表の選択に使う) */

    RpgActor actor;    /* level/exp/hp/max_hp/atk/def/spd/mag/status の実体 */

    u32 gold;
    u16 pos;           /* 盤面上の位置 (マスID)。マスは最大384なので u16 */

    InvBag bag;        /* libos32inv のインベントリバッグ */
    u8 is_cpu;

    u8 is_devil;       /* 1=デビルマン変身中, 0=通常 */
    u16 devil_turns;   /* 残り変身ターン数 */
    u8 devil_cooldown; /* 再変身可能になるまでのクールダウン週数 */

    /* デビル変身前の素のパラメータ (変身解除時に厳密に復元するため保持) */
    i16 base_atk, base_def, base_spd, base_mag, base_max_hp;
} GluePlayer;

/* グローバル変数の宣言 */
extern GluePlayer glue_players[GLUE_MAX_PLAYERS];
extern u8 glue_num_players;

/* 手番スケジューラ (libos32turn)。glue_current_player / glue_week /
   glue_turn はこの状態から導出されるミラーで、glue_turn_advance() が更新する。
   直接書き換えないこと。 */
extern TurnState glue_turnstate;
extern u8 glue_current_player;
extern u16 glue_week;
extern u16 glue_turn;

/* 直近の週境界で各プレイヤーが回収した上納金 (UI表示用) */
extern u32 glue_last_income[GLUE_MAX_PLAYERS];

/* 手番を次のプレイヤーへ進める。
   週境界を跨いだ時は上納金の蓄積・デビルクールダウン減算まで行う。
   out_crossed_week に週境界を跨いだか (1/0) を返す (NULL 可)。
   戻り値: 1=ゲーム終了条件成立, 0=継続 */
int glue_turn_advance(int *out_crossed_week);

/* 週境界処理のみを単体で実行する (テスト・イベント用) */
void glue_week_tick(void);

/* グルーインターフェース関数 */
int glue_init(KernelAPI *kapi);
void glue_shutdown(void);

/* プレイヤーの実効ステータス（装備品補正込み） */
int glue_player_effective_atk(int pid);
int glue_player_effective_def(int pid);

/* 敵マスタ */
/* 読み込み済み敵の件数 (0 = battle.db に enemies テーブルがない) */
int glue_enemy_count(void);
/* 敵ID から敵マスタを引く。未登録なら NULL */
const GlueEnemy *glue_enemy_get(u16 enemy_id);
/* 敵ID の表示名。未登録なら "Monster" */
const char *glue_enemy_name(u16 enemy_id);
/* ステージの野生敵をランダムに 1 体選ぶ。該当なしなら 0 */
u16 glue_enemy_pick_wild(u8 stage);
/* ステージのボスID。該当なしなら 0 */
u16 glue_enemy_boss_of_stage(u8 stage);
/* マスに出現する野生敵をランダムに 1 体選ぶ (マスの area をステージとみなす) */
u16 glue_enemy_pick_for_mass(u16 mass_id);
/* 他プレイヤーの村に攻め込む時の守備側。守備モンスターが居ればそのID、
   居なければその村のステージの野生敵 */
u16 glue_enemy_village_guard(u16 mass_id);

/* ---- 戦闘コマンド ----
   battle.db の command_matrix は [攻撃コマンド][防御コマンド] を引く表。
   攻撃側と防御側が別々に選ぶのが本来の形なので、役割を明示して解決する。 */
#define GLUE_ATK_NORMAL  0   /* 攻撃 */
#define GLUE_ATK_HEAVY   1   /* 強攻撃 (防御を無視するが反射に弱い) */
#define GLUE_ATK_MAGIC   2   /* 魔法 (無防備に大きい。回避されにくい) */
#define GLUE_ATK_CHARGE  3   /* ためる (ダメージなし。次の攻撃を強化) */
#define GLUE_ATK_FLEE    4   /* 逃走 */
#define GLUE_ATK_COUNT   5

#define GLUE_DEF_NONE    0   /* 無防備 */
#define GLUE_DEF_GUARD   1   /* 防御 */
#define GLUE_DEF_DODGE   2   /* 回避 */
#define GLUE_DEF_REFLECT 3   /* 反射 */
#define GLUE_DEF_COUNT   4

/* コマンド名 (UI表示用) */
const char *glue_atk_cmd_name(u8 cmd);
const char *glue_def_cmd_name(u8 cmd);

/* 攻撃側 -> 防御側 の1撃を解決し、ダメージを defender へ適用する。
   戻り値: 0=通常, 1=逃走成功 */
int glue_battle_strike(BtlUnit *atk, u8 atk_cmd,
                       BtlUnit *def, u8 def_cmd, BtlResult *out);

/* CPU の攻撃/防御コマンド選択 (libos32ai の性格プロファイルを使う) */
u8 glue_ai_attack_cmd(int pid, const BtlUnit *self, const BtlUnit *foe);
u8 glue_ai_defend_cmd(int pid, const BtlUnit *self, const BtlUnit *foe);

/* バトル開始処理: プレイヤー vs モンスター */
int glue_battle_start_pve(int pid, u16 monster_id, BtlUnit *player_unit, BtlUnit *enemy_unit);
/* バトルターン解決: コマンドを入力して1ターン処理 */
int glue_battle_resolve_turn(BtlUnit *player_unit, u8 player_cmd, BtlUnit *enemy_unit, u8 enemy_cmd, BtlResult *res_p, BtlResult *res_e);

/* 村経済処理 */
/* マスID -> 村マスタID (econ.db estates.id)。村マスでなければ 0 を返す。
   board.db の masses.param が村ID 1〜59 を保持している (param=0 = 村なし) */
u16 glue_village_id_of_mass(u16 mass_id);
/* マスの村不動産。村マスでない/マスタ未登録なら NULL */
const EconEstate *glue_village_estate(u16 mass_id);
/* マスの村の投資費用。村マスでなければ 0 */
u32 glue_village_invest_cost(u16 mass_id);
int glue_village_claim(u16 mass_id, int pid);
int glue_village_invest(u16 mass_id, int pid);
/* 統治村に溜まった上納金を回収し、プレイヤーの所持金に加算する。
   戻り値は回収額 */
u32 glue_village_collect_taxes(int pid);
u32 glue_player_total_assets(int pid);

/* ---- 勝利条件 ----
   3種の王座のいずれかで決着する:
     資産王:   ターン上限 (GLUE_MAX_WEEKS 週) 到達時に総資産が最大
     制覇王:   全村の過半数 (30/59) を統治した瞬間
     征服王:   城のボス街道を登り切り、最終ボス (stage 8) を撃破した瞬間
   城 (MASS_CASTLE) には glue_boss_progress 段のボスが常駐しており、
   止まると挑戦できる。討伐指令 (BOSS HUNT) 週の撃破には賞金が上乗せ。 */
#define GLUE_MAX_WEEKS      16
#define GLUE_WIN_NONE       0
#define GLUE_WIN_ASSET      1   /* 資産王 */
#define GLUE_WIN_DOMINATION 2   /* 制覇王 */
#define GLUE_WIN_CONQUEST   3   /* 征服王 */
extern int glue_victory_type;    /* GLUE_WIN_* (NONE = 続行中) */
extern int glue_victory_winner;  /* 勝者 pid (-1 = 未決着) */
extern u8  glue_boss_progress;   /* 城で待つボスの stage (1..8, 9=全滅済) */
/* 勝利種別の表示名 */
const char *glue_victory_name(int type);

/* ---- 言霊 (魔法アイテム) ----
   items.db の消耗品のうち effect が 10 番台のもの。戦闘の「魔法」コマンドで
   持っている言霊を選んで唱える。唱えると 1 個消費する。
   1つも持っていない場合は従来どおり MAG 依存の素の魔法攻撃になる。 */
#define GLUE_KOTODAMA_ATTACK  10   /* param + 術者MAG のダメージ */
#define GLUE_KOTODAMA_HEAL    11   /* param HP 回復 */
#define GLUE_KOTODAMA_CURSE   12   /* stat_bonus の状態異常を付与 */
#define GLUE_MAX_KOTODAMA     8

/* アイテムIDが言霊か */
int glue_is_kotodama(u16 item_id);
/* バッグの中の言霊を列挙する。out_slots にはバッグのスロット番号が入る。
   戻り値=件数 */
int glue_kotodama_list(int pid, u8 *out_slots, int max);
/* 言霊を唱える。slot はバッグのスロット番号。
   self/foe は戦闘中のユニット。msg には結果の説明が入る (64バイト以上)。
   戻り値: 0=成功 (1個消費済み), -1=失敗 */
int glue_kotodama_cast(int pid, u8 slot, BtlUnit *self, BtlUnit *foe,
                       char *msg);

/* ---- ダンジョン (潜行型) ----
   入口マスに止まると潜行が始まる。1階ごとに出来事が1つ起こり、
   降りるほど敵は強くなるが戦利品も増える。いつでも撤退でき、
   撤退すると溜めた戦利品を持ち帰れる。倒れると全部失う。
   別マップを持たず、フロアの抽選と戦利品の勘定だけで表現する。 */
#define GLUE_DUNGEON_MAX_FLOOR  10
#define GLUE_DFLOOR_BATTLE   0   /* 魔物 (戦闘へ) */
#define GLUE_DFLOOR_TREASURE 1   /* 財宝 (戦利品 + アイテム) */
#define GLUE_DFLOOR_TRAP     2   /* 罠 (ダメージ) */
#define GLUE_DFLOOR_SHRINE   3   /* 祠 (回復) */
#define GLUE_DFLOOR_EXIT     4   /* 最深部に到達 (強制帰還) */

extern int glue_dungeon_floor;   /* 現在の階 (1 起点。0 = 潜行していない) */
extern u32 glue_dungeon_loot;    /* 持ち帰り待ちの戦利品 (G) */

/* 潜行を開始する */
void glue_dungeon_enter(int pid);
/* 1階降りる。起きた出来事 (GLUE_DFLOOR_*) を返し、msg に説明を書く。
   GLUE_DFLOOR_BATTLE のときは out_enemy に敵IDが入る (呼び出し側で戦闘へ)。
   msg は 64 バイト以上 */
int  glue_dungeon_descend(int pid, u16 *out_enemy, char *msg);
/* 戦闘に勝った/罠を抜けた後、その階の戦利品を加算する */
void glue_dungeon_reward_floor(int pid);
/* 撤退して戦利品を持ち帰る。戻り値=持ち帰った額 */
u32  glue_dungeon_escape(int pid);
/* 倒れて戦利品を失う */
void glue_dungeon_fail(int pid);

/* ---- 盤上でのアイテム使用 ----
   回復薬・毒消しなどを持ち物画面から使う。言霊は戦闘専用なので対象外。
   msg には結果の説明が入る (64バイト以上)。
   戻り値: 0=使った (1個消費), -1=使えないアイテム */
int glue_use_item(int pid, u8 slot, char *msg);
/* 城に常駐する現在のボスの敵ID。全ボス撃破済みなら 0 */
u16 glue_castle_boss(void);
/* 城ボス撃破の後処理: 賞金・進行・征服王判定。戻り値=賞金額 */
u32 glue_boss_defeated(int pid);
/* 制覇王/資産王の判定。決着したら glue_victory_* を埋めて 1 を返す */
int glue_victory_check(void);

/* ---- 集金所 ----
   村マスでの統治/投資はそのまま残しつつ、集金所では「統治済みの村を
   一覧から選んで集金 or 投資」できる。遠くの村を育てる手段になる。 */
#define GLUE_MAX_OWNED ECON_ESTATE_MAX
/* 統治中の村マスタIDを列挙する。戻り値=件数 */
int glue_collect_list(int pid, u16 *out, int max);
/* 指定した村に溜まった上納金だけを回収し所持金へ加算。戻り値=回収額 */
u32 glue_collect_one(int pid, u16 estate_id);
/* 指定した村へ投資する。戻り値: 0=成功, -1=不正, -2=所持金不足 */
int glue_invest_estate(int pid, u16 estate_id);
/* 指定した村の投資費用 */
u32 glue_invest_cost(u16 estate_id);

/* ---- 曜日 ----
   1週=7ターン。ターン数から曜日を導く。
   土曜は買値25%引き、日曜は休業 (ドカポン3・2・1 の店に倣う)。 */
#define GLUE_SUN 0
#define GLUE_MON 1
#define GLUE_TUE 2
#define GLUE_WED 3
#define GLUE_THU 4
#define GLUE_FRI 5
#define GLUE_SAT 6
/* 現在の曜日 (GLUE_SUN..GLUE_SAT) */
int glue_day_of_week(void);
/* 曜日名 ("SUN".."SAT") */
const char *glue_day_name(void);
/* 今日の買値倍率 (百分率)。0 = 休業 */
u16 glue_shop_price_scale(void);
/* 曜日に応じた買値倍率を libos32inv へ反映する。手番が進むたびに呼ぶ */
void glue_shop_apply_day(void);

/* ---- 週次イベント ----
   週境界 (7ターンごと) に evt_tick() を回す。
   evt_tick に渡す current_turn は「週数」なので、events.db の
   min_turn / cooldown / period の単位はすべて週。
   効果の中身は apply_event() が ID で分岐して実装する。 */
#define GLUE_EVT_HARVEST       1
#define GLUE_EVT_TAX_LEVY      2
#define GLUE_EVT_BANDITS       3
#define GLUE_EVT_PLAGUE        4
#define GLUE_EVT_MONSTER_SURGE 5
#define GLUE_EVT_ORACLE        6
#define GLUE_EVT_BIG_SALE      7
#define GLUE_EVT_TREASURE_HUNT 8
#define GLUE_EVT_BOSS_HUNT     9

#define GLUE_EVT_MSG_LEN 160
/* 直近の週境界で発生したイベントの説明 (UI表示用)。空文字なら何も起きていない */
extern char glue_event_msg[GLUE_EVT_MSG_LEN];
/* 魔物大量発生が継続中か (敵のステータスが上がる) */
int glue_event_monster_surge(void);
/* 大売出しが継続中か (店が半額) */
int glue_event_big_sale(void);

/* ---- 地形効果 ----
   毒の沼は「通過しただけで」毒、雪原は「止まったとき」にマヒ。 */
#define GLUE_ST_POISON   0x01
#define GLUE_ST_PARALYZE 0x02
/* terrain (BOARD_TERR_*) の効果を適用する。stopped=1 なら停止時の効果も見る。
   戻り値: 0=何も起きない, GLUE_ST_* = 付与した状態異常 */
u32 glue_apply_terrain(int pid, u8 terrain, int stopped);
/* 状態異常の毎ターン進行 (毒ダメージ・自然回復)。戻り値=受けたダメージ */
int glue_status_tick(int pid);

/* 戦闘報酬: 敵を倒した時の経験値・金銭を加算し、レベルアップを進行させる。
   out_lv に上昇結果を返す (NULL 可)。戻り値: 上昇したレベル数 */
int glue_battle_reward(int pid, u16 monster_id, RpgLevelResult *out_lv);

/* セーブ/ロード */
#define GLUE_SAVE_PATH "/home/user/dokapon.sav"
int glue_save(const char *path);
int glue_load(const char *path);

/* デビルマン変身システム関連 */
int glue_devil_check(int pid);
void glue_devil_transform(int pid);
void glue_devil_release(int pid);
int glue_devil_update(int pid, char *out_msg);

#endif /* GAME_GLUE_H */
