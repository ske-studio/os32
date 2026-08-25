/* ======================================================================== */
/*  GAME_SOUND.C — BGM と効果音                                              */
/* ======================================================================== */

#include "game_sound.h"
#include "libos32snd.h"

static KernelAPI *api;
static int g_scene = -1;
static int g_enabled = 1;

/* ------------------------------------------------------------------ */
/*  BGM (MML)                                                          */
/*                                                                    */
/*  カーネルの MML は 1 パートのみ (@C でチャンネルを指定できるが、    */
/*  ノートは書いた順に鳴る) なので、和音ではなく単旋律で書く。         */
/*  [ ] で囲むとその位置からループする。                               */
/* ------------------------------------------------------------------ */

/* タイトル: 荘重に。神話の導入 */
static const char *MML_TITLE =
    "T96 @C0 @T2 O4 L4 "
    "[ A2 >C< B A2 R "
    "  G2 A B >C2< R "
    "  F2 G A G2 R "
    "  E2 F E D2 R2 ]";

/* フィールド: 軽快な旅の曲 */
static const char *MML_FIELD =
    "T132 @C0 @T0 O4 L8 "
    "[ C E G >C< G E  D F A >D< A F "
    "  E G B >E< B G  D F A F D R "
    "  C E G >C< G E  F A >C< F A >C< "
    "  <G B >D G< B >D<  C4 R4 R4 ]";

/* 戦闘: 速く刻む */
static const char *MML_BATTLE =
    "T168 @C0 @T0 O3 L8 "
    "[ A A >E< A  A A >E< A "
    "  G G >D< G  G G >D< G "
    "  F F >C< F  F F >C< F "
    "  E E >B< E  E4 R4 ]";

/* ボス戦: 低く重い */
static const char *MML_BOSS =
    "T152 @C0 @T1 O2 L8 "
    "[ D D >D< D  D+ D+ >D+< D+ "
    "  C C >C< C  C+ C+ >C+< C+ "
    "  D R D R  F R G R "
    "  A2 >D2< R4 ]";

/* 店: のんびり */
static const char *MML_SHOP =
    "T112 @C0 @T2 O4 L8 "
    "[ C4 E4 G4 E4  F4 A4 >C4< A4 "
    "  <B4 >D4 G4 D4  C2 R2 ]";

/* ダンジョン: 不穏に半音で漂う */
static const char *MML_DUNGEON =
    "T104 @C0 @T1 O3 L8 "
    "[ D R D+ R  E R D+ R "
    "  D R C R  <B R >C R "
    "  D2 R2  <A+2 R2 ]";

/* 決着: 短いファンファーレのあとループ */
static const char *MML_RESULT =
    "T120 @C0 @T2 O4 L8 "
    "[ C C C4 E4 G4  >C2< R4 "
    "  G4 E4 C4 E4  G2 R4 ]";

static const char *scene_mml(int scene)
{
    switch (scene) {
    case GS_BGM_TITLE:   return MML_TITLE;
    case GS_BGM_FIELD:   return MML_FIELD;
    case GS_BGM_BATTLE:  return MML_BATTLE;
    case GS_BGM_BOSS:    return MML_BOSS;
    case GS_BGM_SHOP:    return MML_SHOP;
    case GS_BGM_DUNGEON: return MML_DUNGEON;
    case GS_BGM_RESULT:  return MML_RESULT;
    default:             return (const char *)0;
    }
}

void game_sound_init(KernelAPI *kapi)
{
    api = kapi;
    libos32snd_init(kapi);
    snd_set_master(1);
    /* ゲームを抜けたら音も止める */
    snd_bgm_set_persist(0);
    g_scene = -1;
    g_enabled = 1;
}

void game_sound_scene(int scene)
{
    const char *mml;

    if (!g_enabled) return;
    if (scene == g_scene) return;

    g_scene = scene;
    mml = scene_mml(scene);
    if (!mml) {
        snd_bgm_stop();
    } else {
        snd_bgm_play(mml);
    }
}

void game_sound_se(int se)
{
    if (!g_enabled) return;

    /* SE は FM Ch2 / SSG ChC を一時借用する。BGM は自動で復帰する。
       音程と長さでゲームらしい鳴り分けを作る (音色 0=ピアノ 1=ベル) */
    switch (se) {
    case GS_SE_DICE:      snd_se_play_custom(SND_NOTE(5, SND_N_G),  60, 0); break;
    case GS_SE_MOVE:      snd_se_play_custom(SND_NOTE(5, SND_N_C),  40, 0); break;
    case GS_SE_HIT:       snd_se_play_custom(SND_NOTE(3, SND_N_G),  80, 0); break;
    case GS_SE_DAMAGE:    snd_se_play_custom(SND_NOTE(2, SND_N_A), 120, 0); break;
    case GS_SE_WIN:       snd_se_play_custom(SND_NOTE(6, SND_N_C), 200, 1); break;
    case GS_SE_LOSE:      snd_se_play_custom(SND_NOTE(2, SND_N_C), 300, 0); break;
    case GS_SE_COIN:      snd_se_play(SND_SE_COIN);                         break;
    case GS_SE_ITEM:      snd_se_play_custom(SND_NOTE(6, SND_N_E), 120, 1); break;
    case GS_SE_SELECT:    snd_se_play(SND_SE_SELECT);                       break;
    case GS_SE_CANCEL:    snd_se_play(SND_SE_CANCEL);                       break;
    case GS_SE_KOTODAMA:  snd_se_play_custom(SND_NOTE(6, SND_N_A), 250, 1); break;
    default:                                                                break;
    }
}

int game_sound_toggle(void)
{
    g_enabled = !g_enabled;
    if (g_enabled) {
        int scene = g_scene;
        snd_set_master(1);
        /* 消音中に変わった場面を鳴らし直す */
        g_scene = -1;
        game_sound_scene(scene);
    } else {
        snd_bgm_stop();
        snd_set_master(0);
    }
    return g_enabled;
}

int game_sound_enabled(void)
{
    return g_enabled;
}

void game_sound_shutdown(void)
{
    snd_bgm_stop();
    snd_set_master(1);
}
