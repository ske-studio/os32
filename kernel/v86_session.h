/* ======================================================================== */
/*  V86_SESSION.H - V86 セッションマネージャ                                */
/*                                                                          */
/*  V86 セッションのライフサイクル管理を行う。                              */
/*  イメージ読み込み、V86コンテキスト構築、Auto-Typer、終了処理を統括する。 */
/* ======================================================================== */

#ifndef V86_SESSION_H
#define V86_SESSION_H

#include "types.h"

/* ====================================================================== */
/*  V86セッション終了理由                                                  */
/* ====================================================================== */
enum v86_exit_reason {
    V86_EXIT_NONE = 0,
    V86_EXIT_TRAP_PORT,    /* ポート0xFEトラップ (VDOSQUIT.COM) */
    V86_EXIT_DOS_TERM,     /* INT 20h / INT 21h AH=4Ch */
    V86_EXIT_REBOOT,       /* F0hポートリブート検知 */
    V86_EXIT_BIOS_ROM,     /* CS >= 0xF000 (リセットベクタ到達) */
    V86_EXIT_TIMEOUT,      /* タイムアウト */
    V86_EXIT_UNKNOWN_OP,   /* 未対応オペコード */
    V86_EXIT_HOTKEY        /* 強制脱出キー (Ctrl+GRPH+DEL / STOP) */
};

/* ====================================================================== */
/*  BDA キーボードバッファ定数 (PC-98) — v86_bda.h に定義集約               */
/* ====================================================================== */
#include "v86_bda.h"

/* ====================================================================== */
/*  Auto-Typer 定数                                                        */
/* ====================================================================== */
#define V86_AUTO_TYPE_DELAY    400   /* 初期ディレイ (ticks, 約4秒) */
#define V86_AUTO_TYPE_INTERVAL 5     /* 5ticks/文字 = 20文字/秒 */

/* ====================================================================== */
/*  強制脱出ホットキー: Ctrl+GRPH+DEL                                     */
/*  (PC-98の GRPH = PC/AT の Alt 相当)                                    */
/*  F12はNP21/Wのショートカットキーとバッティングするため変更              */
/* ====================================================================== */
#define V86_HOTKEY_SCANCODE  0x39   /* DEL キー */

/* ====================================================================== */
/*  セッション構造体                                                       */
/* ====================================================================== */
typedef struct {
    int fd;                       /* ディスクイメージfd */
    u32 img_offset;               /* イメージデータ先頭オフセット */
    u32 img_data_size;            /* イメージデータサイズ */
    const char *auto_cmd;         /* Auto-Typerコマンド文字列 (NULL=無効) */
    int auto_cmd_idx;             /* Auto-Typer現在位置 */
    u32 auto_delay_remaining;     /* Auto-Typer開始ディレイ残りtick */
    int auto_done;                /* Auto-Typer完了フラグ */
    enum v86_exit_reason exit_reason;  /* 終了理由 */
} V86Session;

/* ====================================================================== */
/*  公開API                                                                */
/* ====================================================================== */

/* ディスクイメージからV86セッションを起動する (統合ブート関数) */
int  v86_boot_image_kapi(const char *path, const char *cmdline);

/* ネイティブPC-98ソフトのFDDイメージからV86セッションを起動する
 * 脱出は Ctrl+GRPH+DEL ホットキーのみ。 */
int  v86_boot_native(const char *path, const char *cmdline);

/* 実FDDからV86セッションを起動する (ネイティブモード)
 * drv: 物理ドライブ番号 (通常0) */
int  v86_boot_physical_fdd(int drv, const char *cmdline);

/* IRQ0 (100Hz) ごとに呼ばれるコールバック
 * Auto-Typer処理と強制脱出ホットキー検知を行う */
void v86_session_on_tick(void);

/* V86終了を要求する (理由を記録) */
void v86_request_exit(enum v86_exit_reason reason);

/* 終了理由を文字列に変換 */
const char *v86_exit_reason_str(enum v86_exit_reason reason);

/* V86終了要求フラグ (v86.cから参照) */
extern volatile int v86_exit_request;

/* IRQ0注入カウンタ (v86.cから参照) */
extern u32 v86_irq0_inject_count;

#endif /* V86_SESSION_H */
