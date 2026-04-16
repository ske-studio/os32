/* ======================================================================== */
/*  SND_ENGINE.C — サウンドエンジン本体                                     */
/*                                                                          */
/*  timer_handler (IRQ0, 100Hz) から snd_tick() を呼び出し、                */
/*  BGM の各ノートを非ブロッキングで再生する。                              */
/*  SE 発火時は FM Ch2 / SSG ChC を一時借用し、終了後にBGM復帰。           */
/* ======================================================================== */

#include "snd_engine.h"
#include "fm.h"
#include "io.h"

/* ======================================================================== */
/*  外部参照                                                                */
/* ======================================================================== */

extern volatile u32 tick_count;

/* ======================================================================== */
/*  内部定義                                                                */
/* ======================================================================== */

/* ノート文字→音名インデックス変換テーブル */
/*  C=0, D=2, E=4, F=5, G=7, A=9, B=11 */
static const int note_char_map[7] = {
    9, 11, 0, 2, 4, 5, 7   /* A, B, C, D, E, F, G */
};

/* SSG周波数: ノート番号→period 簡易テーブル (オクターブ4基準) */
/* Period = 62400 / freq, freq = 440 * 2^((note-57)/12) */
static const u16 ssg_period_base[12] = {
    956,  902,  851,  804,  758,  716,
    676,  638,  602,  568,  536,  506
};

/* チャンネル借用状態 */
typedef struct {
    int  borrowed;           /* 1 = SEが借用中 */
    u8   saved_tone;         /* 保存したBGM音色番号 */
    u8   saved_note;         /* 保存したBGMノート番号 */
    int  saved_was_sounding; /* 借用時にBGMが鳴っていたか */
} SndBorrowState;

/* FM SE スロット */
typedef struct {
    int  active;
    u32  end_tick;
} SndFmSE;

/* SSG SE スロット */
typedef struct {
    int  active;
    u32  end_tick;
    u8   ssg_vol;
    u8   ssg_decay;
} SndSsgSE;

/* BGMトラック */
typedef struct {
    SndNote notes[SND_MAX_NOTES];
    int     num_notes;
    int     playing;
    int     pos;
    u32     next_tick;
    int     loop_pos;        /* ループ開始位置 (-1=なし) */
} SndBGMTrack;

/* SE定義 */
typedef struct {
    u8  use_fm;
    u8  tone_num;
    u8  note;
    u8  duration;
    u16 ssg_period;
    u8  ssg_vol;
    u8  ssg_decay;
} SndSE_Def;

/* サウンドエンジン統合構造体 */
typedef struct {
    SndBGMTrack     bgm;
    SndFmSE         fm_se;
    SndSsgSE        ssg_se;
    SndBorrowState  fm_borrow;
    SndBorrowState  ssg_borrow;
    SndSE_Def       se_table[SND_SE_MAX];
    int             master_enable;
    int             bgm_persist;     /* 1=exec_exit時にBGMを停止しない */
} SndEngine;

static SndEngine g_snd;

/* ======================================================================== */
/*  SSG ノート→Period 変換                                                 */
/* ======================================================================== */

static u16 ssg_period_from_note(int note)
{
    int octave = note / 12;
    int key = note % 12;
    u16 base = ssg_period_base[key];
    int shift;

    /* オクターブ4基準。4以上なら右シフト(高音)、4未満なら左シフト(低音) */
    shift = octave - 4;
    if (shift > 0) {
        while (shift > 0) { base >>= 1; shift--; }
    } else if (shift < 0) {
        while (shift < 0) { base <<= 1; shift++; }
    }
    if (base < 1) base = 1;
    return base;
}

/* ======================================================================== */
/*  チャンネル借用判定                                                      */
/* ======================================================================== */

static int snd_ch_is_borrowed(int ch)
{
    if (ch == SND_FM_SE_CH && g_snd.fm_borrow.borrowed) return 1;
    if (ch == SND_SSG_SE_LOGICAL && g_snd.ssg_borrow.borrowed) return 1;
    return 0;
}

/* ======================================================================== */
/*  BGM: 指定ポジションのノートを発音開始する内部関数                        */
/* ======================================================================== */

static void snd_bgm_start_note(SndBGMTrack *bgm)
{
    SndNote *note = &bgm->notes[bgm->pos];

    /* ループマーカーをスキップ */
    while (note->note == SND_LOOP_MARK && bgm->pos < bgm->num_notes) {
        bgm->pos++;
        note = &bgm->notes[bgm->pos];
    }

    /* 曲末チェック */
    if (bgm->pos >= bgm->num_notes || note->note == SND_END) {
        if (bgm->loop_pos >= 0) {
            bgm->pos = bgm->loop_pos;
            note = &bgm->notes[bgm->pos];
            /* ループ先頭のマーカーもスキップ */
            while (note->note == SND_LOOP_MARK && bgm->pos < bgm->num_notes) {
                bgm->pos++;
                note = &bgm->notes[bgm->pos];
            }
        } else {
            bgm->playing = 0;
            fm_all_off();
            ssg_all_off();
            return;
        }
    }

    /* 発音 or 休符 */
    if (note->note != SND_REST) {
        if (!snd_ch_is_borrowed(note->channel)) {
            if (note->channel < 3) {
                fm_set_tone_num(note->channel, note->tone);
                fm_note_on(note->channel, note->note);
            } else {
                int ssg_ch = note->channel - 3;
                ssg_tone(ssg_ch, ssg_period_from_note(note->note));
                ssg_volume(ssg_ch, 15);
            }
        }
        /* 借用中でも保存情報を更新 */
        if (note->channel == SND_FM_SE_CH &&
            g_snd.fm_borrow.borrowed) {
            g_snd.fm_borrow.saved_tone = note->tone;
            g_snd.fm_borrow.saved_note = note->note;
            g_snd.fm_borrow.saved_was_sounding = 1;
        }
        if (note->channel == SND_SSG_SE_LOGICAL &&
            g_snd.ssg_borrow.borrowed) {
            g_snd.ssg_borrow.saved_tone = note->tone;
            g_snd.ssg_borrow.saved_note = note->note;
            g_snd.ssg_borrow.saved_was_sounding = 1;
        }
    } else {
        /* 休符: 借用チャンネルの保存状態を更新 */
        if (note->channel == SND_FM_SE_CH &&
            g_snd.fm_borrow.borrowed) {
            g_snd.fm_borrow.saved_was_sounding = 0;
        }
        if (note->channel == SND_SSG_SE_LOGICAL &&
            g_snd.ssg_borrow.borrowed) {
            g_snd.ssg_borrow.saved_was_sounding = 0;
        }
    }

    /* 次のノート切替時刻を設定 */
    bgm->next_tick = tick_count + note->duration;
}

/* ======================================================================== */
/*  BGM tick 処理                                                           */
/* ======================================================================== */

static void snd_bgm_tick(void)
{
    SndBGMTrack *bgm = &g_snd.bgm;
    SndNote *cur;

    /* まだ現在のノートの持続時間中 */
    if (tick_count < bgm->next_tick) return;

    /* 現在のノートを停止 (前のノートの発音を止める) */
    cur = &bgm->notes[bgm->pos];
    if (cur->note != SND_REST && cur->note != SND_END &&
        cur->note != SND_LOOP_MARK) {
        if (!snd_ch_is_borrowed(cur->channel)) {
            if (cur->channel < 3) {
                fm_note_off(cur->channel);
            } else {
                ssg_volume(cur->channel - 3, 0);
            }
        }
    }

    /* 次のノートへ進む */
    bgm->pos++;

    /* 次のノートを発音開始 */
    snd_bgm_start_note(bgm);
}

/* ======================================================================== */
/*  FM SE tick (終了判定 + チャンネル返却)                                   */
/* ======================================================================== */

static void snd_fm_se_tick(void)
{
    if (!g_snd.fm_se.active) return;

    if (tick_count >= g_snd.fm_se.end_tick) {
        fm_note_off(SND_FM_SE_CH);
        g_snd.fm_se.active = 0;

        /* FM Ch2 をBGMに返却 */
        if (g_snd.fm_borrow.borrowed) {
            g_snd.fm_borrow.borrowed = 0;
            if (g_snd.fm_borrow.saved_was_sounding &&
                g_snd.bgm.playing) {
                fm_set_tone_num(SND_FM_SE_CH,
                                g_snd.fm_borrow.saved_tone);
                fm_note_on(SND_FM_SE_CH,
                           g_snd.fm_borrow.saved_note);
            }
        }
    }
}

/* ======================================================================== */
/*  SSG SE tick (減衰 + 終了判定 + チャンネル返却)                           */
/* ======================================================================== */

static void snd_ssg_se_tick(void)
{
    if (!g_snd.ssg_se.active) return;

    /* 減衰処理 */
    if (g_snd.ssg_se.ssg_decay > 0) {
        if (g_snd.ssg_se.ssg_vol > g_snd.ssg_se.ssg_decay) {
            g_snd.ssg_se.ssg_vol -= g_snd.ssg_se.ssg_decay;
        } else {
            g_snd.ssg_se.ssg_vol = 0;
        }
        ssg_volume(SND_SSG_SE_CH, g_snd.ssg_se.ssg_vol);
    }

    if (tick_count >= g_snd.ssg_se.end_tick) {
        ssg_volume(SND_SSG_SE_CH, 0);
        g_snd.ssg_se.active = 0;

        /* SSG ChC をBGMに返却 */
        if (g_snd.ssg_borrow.borrowed) {
            g_snd.ssg_borrow.borrowed = 0;
            if (g_snd.ssg_borrow.saved_was_sounding &&
                g_snd.bgm.playing) {
                ssg_tone(SND_SSG_SE_CH,
                         ssg_period_from_note(
                             g_snd.ssg_borrow.saved_note));
                ssg_volume(SND_SSG_SE_CH, 15);
            }
        }
    }
}

/* ======================================================================== */
/*  MMLパーサ — MML文字列をSndNote配列に変換                               */
/* ======================================================================== */

static int parse_mml_number(const char **pp)
{
    int val = 0;
    while (**pp >= '0' && **pp <= '9') {
        val = val * 10 + (**pp - '0');
        (*pp)++;
    }
    return val;
}

static int snd_mml_parse(const char *mml, SndNote *out, int max_notes,
                          int *out_loop_pos)
{
    int pos = 0;
    int octave = 4;
    int default_dur = 50;   /* 4分音符 = 500ms @ 120BPM */
    int channel = 0;
    int tone = 0;
    int loop_pos = -1;

    while (*mml && pos < max_notes - 1) {
        char ch = *mml++;

        /* スペース・カンマ・クォート → スキップ */
        if (ch == ' ' || ch == ',' || ch == '\n' || ch == '\r' || ch == '"')
            continue;

        /* テンポ: T120 */
        if (ch == 'T' || ch == 't') {
            int bpm = parse_mml_number(&mml);
            if (bpm > 0) {
                default_dur = 6000 / bpm;
                if (default_dur < 1) default_dur = 1;
            }
            continue;
        }

        /* オクターブ: O4 */
        if (ch == 'O' || ch == 'o') {
            if (*mml >= '1' && *mml <= '8') {
                octave = *mml - '0';
                mml++;
            }
            continue;
        }

        /* オクターブ上下: > < */
        if (ch == '>') { if (octave < 8) octave++; continue; }
        if (ch == '<') { if (octave > 1) octave--; continue; }

        /* 音符長: L4, L8 */
        if (ch == 'L' || ch == 'l') {
            int len = parse_mml_number(&mml);
            if (len > 0) {
                default_dur = 200 / len;  /* 全音符=200tick基準 */
                if (default_dur < 1) default_dur = 1;
            }
            continue;
        }

        /* チャンネル/音色: @C0, @T1 */
        if (ch == '@') {
            if (*mml == 'C' || *mml == 'c') {
                mml++;
                channel = parse_mml_number(&mml);
                if (channel > 5) channel = 0;
            } else if (*mml == 'T' || *mml == 't') {
                mml++;
                tone = parse_mml_number(&mml);
            }
            continue;
        }

        /* ループ開始: [ */
        if (ch == '[') {
            loop_pos = pos;
            out[pos].note = SND_LOOP_MARK;
            out[pos].duration = 0;
            out[pos].channel = 0;
            out[pos].tone = 0;
            pos++;
            continue;
        }

        /* ループ終了: ] → 無視 (loop_posをセットするだけ) */
        if (ch == ']') {
            continue;
        }

        /* 休符: R */
        if (ch == 'R' || ch == 'r') {
            int dur = default_dur;
            int num = parse_mml_number(&mml);
            if (num > 0) {
                dur = 200 / num;
                if (dur < 1) dur = 1;
            }
            out[pos].note = SND_REST;
            out[pos].duration = (u8)(dur > 255 ? 255 : dur);
            out[pos].channel = (u8)channel;
            out[pos].tone = 0;
            pos++;
            continue;
        }

        /* 音名: A-G, a-g */
        if ((ch >= 'A' && ch <= 'G') || (ch >= 'a' && ch <= 'g')) {
            int idx;
            int sharp = 0;
            int dur = default_dur;
            int num;

            if (ch >= 'a') ch = ch - 'a' + 'A';
            idx = note_char_map[ch - 'A'];

            /* シャープ */
            if (*mml == '+' || *mml == '#') {
                sharp = 1;
                mml++;
            }

            /* 個別の音符長 */
            num = parse_mml_number(&mml);
            if (num > 0) {
                dur = 200 / num;
                if (dur < 1) dur = 1;
            }

            /* 付点 */
            if (*mml == '.') {
                dur = dur + dur / 2;
                mml++;
            }

            out[pos].note = (u8)(octave * 12 + idx + sharp);
            out[pos].duration = (u8)(dur > 255 ? 255 : dur);
            out[pos].channel = (u8)channel;
            out[pos].tone = (u8)tone;
            pos++;
            continue;
        }
    }

    /* 終端マーカー */
    out[pos].note = SND_END;
    out[pos].duration = 0;
    pos++;

    *out_loop_pos = loop_pos;
    return pos;
}

/* ======================================================================== */
/*  SE テーブル初期化                                                       */
/* ======================================================================== */

static void snd_init_se_table(void)
{
    SndSE_Def *t = g_snd.se_table;
    int i;

    /* 全エントリをゼロクリア */
    for (i = 0; i < SND_SE_MAX; i++) {
        t[i].use_fm = 0;
        t[i].tone_num = 0;
        t[i].note = 0;
        t[i].duration = 0;
        t[i].ssg_period = 0;
        t[i].ssg_vol = 0;
        t[i].ssg_decay = 0;
    }

    /* SE_CURSOR: SSG 短いピッ */
    t[SND_SE_CURSOR].use_fm = 0;
    t[SND_SE_CURSOR].ssg_period = 200;
    t[SND_SE_CURSOR].ssg_vol = 12;
    t[SND_SE_CURSOR].ssg_decay = 3;
    t[SND_SE_CURSOR].duration = 5;    /* 50ms */

    /* SE_SELECT: FM 決定音 (ベル音色, C6) */
    t[SND_SE_SELECT].use_fm = 1;
    t[SND_SE_SELECT].tone_num = 1;    /* ベル */
    t[SND_SE_SELECT].note = NOTE(6, N_C);
    t[SND_SE_SELECT].duration = 15;   /* 150ms */

    /* SE_CANCEL: SSG 低音ブッ */
    t[SND_SE_CANCEL].use_fm = 0;
    t[SND_SE_CANCEL].ssg_period = 800;
    t[SND_SE_CANCEL].ssg_vol = 15;
    t[SND_SE_CANCEL].ssg_decay = 2;
    t[SND_SE_CANCEL].duration = 8;    /* 80ms */

    /* SE_ERROR: SSG ブブッ (低いノイズ的) */
    t[SND_SE_ERROR].use_fm = 0;
    t[SND_SE_ERROR].ssg_period = 1200;
    t[SND_SE_ERROR].ssg_vol = 15;
    t[SND_SE_ERROR].ssg_decay = 1;
    t[SND_SE_ERROR].duration = 15;    /* 150ms */

    /* SE_COIN: FM 高音ピコ (ベル, E7) */
    t[SND_SE_COIN].use_fm = 1;
    t[SND_SE_COIN].tone_num = 1;      /* ベル */
    t[SND_SE_COIN].note = NOTE(7, N_E);
    t[SND_SE_COIN].duration = 10;     /* 100ms */

    /* SE_BEEP: SSG ビープ */
    t[SND_SE_BEEP].use_fm = 0;
    t[SND_SE_BEEP].ssg_period = 400;
    t[SND_SE_BEEP].ssg_vol = 15;
    t[SND_SE_BEEP].ssg_decay = 0;
    t[SND_SE_BEEP].duration = 10;     /* 100ms */
}

/* ======================================================================== */
/*  FM SE 発火                                                              */
/* ======================================================================== */

static void snd_fm_se_start(int tone_num, int note, int duration)
{
    SndBorrowState *borrow = &g_snd.fm_borrow;

    /* 既にSE再生中なら上書き */
    if (g_snd.fm_se.active) {
        fm_note_off(SND_FM_SE_CH);
    }

    /* BGM の FM Ch2 の現在状態を保存 */
    if (!borrow->borrowed && g_snd.bgm.playing) {
        int i;
        borrow->saved_was_sounding = 0;
        for (i = g_snd.bgm.pos; i >= 0; i--) {
            if (g_snd.bgm.notes[i].channel == SND_FM_SE_CH) {
                borrow->saved_tone = g_snd.bgm.notes[i].tone;
                borrow->saved_note = g_snd.bgm.notes[i].note;
                borrow->saved_was_sounding =
                    (g_snd.bgm.notes[i].note != SND_REST);
                break;
            }
        }
    }

    borrow->borrowed = 1;

    fm_note_off(SND_FM_SE_CH);
    fm_set_tone_num(SND_FM_SE_CH, tone_num);
    fm_note_on(SND_FM_SE_CH, note);

    g_snd.fm_se.active = 1;
    g_snd.fm_se.end_tick = tick_count + (u32)duration;
}

/* ======================================================================== */
/*  SSG SE 発火                                                             */
/* ======================================================================== */

static void snd_ssg_se_start(u16 period, u8 vol, u8 decay, int duration)
{
    SndBorrowState *borrow = &g_snd.ssg_borrow;

    /* 既にSE再生中なら上書き */
    if (g_snd.ssg_se.active) {
        ssg_volume(SND_SSG_SE_CH, 0);
    }

    /* BGM の SSG ChC の現在状態を保存 */
    if (!borrow->borrowed && g_snd.bgm.playing) {
        int i;
        borrow->saved_was_sounding = 0;
        for (i = g_snd.bgm.pos; i >= 0; i--) {
            if (g_snd.bgm.notes[i].channel == SND_SSG_SE_LOGICAL) {
                borrow->saved_tone = g_snd.bgm.notes[i].tone;
                borrow->saved_note = g_snd.bgm.notes[i].note;
                borrow->saved_was_sounding =
                    (g_snd.bgm.notes[i].note != SND_REST);
                break;
            }
        }
    }

    borrow->borrowed = 1;

    ssg_volume(SND_SSG_SE_CH, 0);
    ssg_tone(SND_SSG_SE_CH, period);
    ssg_volume(SND_SSG_SE_CH, vol);

    g_snd.ssg_se.active = 1;
    g_snd.ssg_se.end_tick = tick_count + (u32)duration;
    g_snd.ssg_se.ssg_vol = vol;
    g_snd.ssg_se.ssg_decay = decay;
}

/* ======================================================================== */
/*  公開API                                                                 */
/* ======================================================================== */

void snd_init(void)
{
    int i;

    g_snd.master_enable = 1;
    g_snd.bgm_persist = 0;
    g_snd.bgm.playing = 0;
    g_snd.bgm.num_notes = 0;
    g_snd.bgm.pos = 0;
    g_snd.bgm.loop_pos = -1;
    g_snd.fm_se.active = 0;
    g_snd.ssg_se.active = 0;
    g_snd.fm_borrow.borrowed = 0;
    g_snd.ssg_borrow.borrowed = 0;

    for (i = 0; i < SND_SE_MAX; i++) {
        g_snd.se_table[i].duration = 0;
    }

    snd_init_se_table();
}

void snd_tick(void)
{
    if (!g_snd.master_enable) return;

    /* SE 終了チェック (BGM より先に処理) */
    snd_fm_se_tick();
    snd_ssg_se_tick();

    /* BGM 更新 */
    if (g_snd.bgm.playing) {
        snd_bgm_tick();
    }
}

void snd_bgm_play(const char *mml)
{
    int loop_pos = -1;
    int num;
    int len;

    /* 再生中なら停止 */
    if (g_snd.bgm.playing) {
        snd_bgm_stop();
    }

    /* 先頭のクォートをスキップ */
    if (*mml == '"') mml++;

    /* MML をパースしてノート配列に変換 */
    num = snd_mml_parse(mml, g_snd.bgm.notes, SND_MAX_NOTES, &loop_pos);

    /* 末尾のクォート分をチェック (パーサ内の ']' 処理で対応済み) */

    g_snd.bgm.num_notes = num;
    g_snd.bgm.pos = 0;
    g_snd.bgm.loop_pos = loop_pos;
    g_snd.bgm.playing = 1;

    /* OPN初期化 + SSGミキサーを全チャンネルトーンONに設定 */
    opn_init();
    ssg_mixer(0x38);   /* bit0-2=0: ChA/B/CトーンON, bit3-5=1: ノイズOFF */

    /* 最初のノートを即座に発音開始 */
    snd_bgm_start_note(&g_snd.bgm);
}

void snd_bgm_stop(void)
{
    g_snd.bgm.playing = 0;
    g_snd.bgm_persist = 0;
    g_snd.fm_borrow.borrowed = 0;
    g_snd.ssg_borrow.borrowed = 0;
    g_snd.fm_se.active = 0;
    g_snd.ssg_se.active = 0;
    fm_all_off();
    ssg_all_off();
}

int snd_bgm_is_playing(void)
{
    return g_snd.bgm.playing;
}

void snd_se_play(int se_id)
{
    SndSE_Def *def;
    if (se_id < 0 || se_id >= SND_SE_MAX) return;
    if (!g_snd.master_enable) return;

    def = &g_snd.se_table[se_id];
    if (def->duration == 0) return;  /* 未定義 */

    if (def->use_fm) {
        snd_fm_se_start(def->tone_num, def->note, def->duration);
    } else {
        snd_ssg_se_start(def->ssg_period, def->ssg_vol,
                         def->ssg_decay, def->duration);
    }
}

void snd_se_play_raw(int note, int duration_ticks, int tone)
{
    if (!g_snd.master_enable) return;
    if (duration_ticks < 1) duration_ticks = 1;
    if (duration_ticks > 255) duration_ticks = 255;

    snd_fm_se_start(tone, note, duration_ticks);
}

void snd_set_master(int enable)
{
    g_snd.master_enable = enable;
    if (!enable) {
        fm_all_off();
        ssg_all_off();
    }
}

void snd_cleanup(void)
{
    if (!g_snd.bgm_persist) {
        snd_bgm_stop();
    }
    /* persistフラグはここではリセットしない。
       snd_bgm_stop()またはsnd_bgm_set_persist(0)で明示的にクリアされる。
       これにより、BGM再生中に他のプログラムを実行してもBGMが継続する。 */
}

void snd_bgm_set_persist(int persist)
{
    g_snd.bgm_persist = persist;
}

int snd_bgm_get_persist(void)
{
    return g_snd.bgm_persist;
}
