/* ======================================================================== */
/*  SND_ENGINE.H — サウンドエンジン (BGM/SE バックグラウンド再生)           */
/*                                                                          */
/*  timer_handler (IRQ0, 100Hz) から駆動される割り込み駆動型音楽再生系。    */
/*  BGM: FM 3ch + SSG 3ch の全6チャンネルを使用可能。                       */
/*  SE:  FM Ch2 / SSG ChC を一時借用し、終了後にBGMを復帰。               */
/* ======================================================================== */

#ifndef __SND_ENGINE_H
#define __SND_ENGINE_H

#include "types.h"

/* ======================================================================== */
/*  定数                                                                    */
/* ======================================================================== */

#define SND_MAX_NOTES       512   /* BGM最大ノート数 */
#define SND_REST            0xFF  /* 休符マーカー */
#define SND_END             0xFE  /* 曲終了マーカー */
#define SND_LOOP_MARK       0xFD  /* ループ開始マーカー */

/* SE 借用チャンネル (固定) */
#define SND_FM_SE_CH        2     /* FM Ch2 を借用 */
#define SND_SSG_SE_CH       2     /* SSG ChC を借用 */
#define SND_SSG_SE_LOGICAL  5     /* 論理チャンネル番号 (3+2=5) */

/* SE ID 定数 */
#define SND_SE_CURSOR       0     /* カーソル移動: SSG 短いピッ */
#define SND_SE_SELECT       1     /* 決定: FM ピロン */
#define SND_SE_CANCEL       2     /* キャンセル: SSG 低音 */
#define SND_SE_ERROR        3     /* エラー: SSG ブブッ */
#define SND_SE_COIN         4     /* アイテム取得: FM 高音 */
#define SND_SE_BEEP         5     /* BEEP: SSG */
#define SND_SE_MAX          16

/* ======================================================================== */
/*  データ構造                                                              */
/* ======================================================================== */

/* BGMノートイベント (事前パース済み, 4bytes/event) */
typedef struct {
    u8 note;       /* ノート番号 (SND_REST=休符, SND_END=終了) */
    u8 tone;       /* 音色番号 */
    u8 channel;    /* 出力チャンネル (0-2=FM, 3-5=SSG) */
    u8 duration;   /* 持続時間 (tick数, 1tick=10ms) */
} SndNote;

/* ======================================================================== */
/*  公開API                                                                 */
/* ======================================================================== */

/* 初期化 (kernel_main から呼ぶ) */
void snd_init(void);

/* timer_handler から毎tick呼ばれるメインループ */
void snd_tick(void);

/* BGM 制御 */
void snd_bgm_play(const char *mml);
void snd_bgm_stop(void);
int  snd_bgm_is_playing(void);

/* SE 制御 */
void snd_se_play(int se_id);
void snd_se_play_raw(int note, int duration_ticks, int tone);

/* マスター制御 */
void snd_set_master(int enable);

/* BGM持続フラグ (1=exec_exit時にBGMを停止しない) */
void snd_bgm_set_persist(int persist);
int  snd_bgm_get_persist(void);

/* exec_exit 時に呼ばれるクリーンアップ */
void snd_cleanup(void);

#endif /* __SND_ENGINE_H */
