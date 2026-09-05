/* ======================================================================== */
/*  CONFIG.H — OS32 システム全般設定                                         */
/*                                                                          */
/*  システムの挙動、デフォルトファイルパス、各種タイムアウト等の定数を定義します。*/
/* ======================================================================== */

#ifndef CONFIG_H
#define CONFIG_H

/* ====================================================================== */
/*  バージョン                                                              */
/*                                                                          */
/*  OS 全体のバージョン文字列。git のタグ (vX.Y.Z) と歩調を合わせること。   */
/*  ここが唯一の定義で、ver コマンドはこれを表示する。                       */
/* ====================================================================== */
#define SYS_VERSION           "2.0"

/* ====================================================================== */
/*  スタートアップ設定                                                       */
/* ====================================================================== */
#define SYS_SHELL_BIN         "/sys/shell.bin"     /* システムシェル (HDD/FDD共通) */
#define SYS_SHELL_BIN_FDD     "/sys/shell.bin"     /* FDDフォールバック (統一パス) */
#define SYS_PROFILE_SYS       "/etc/profile"       /* システムプロファイル */
#define SYS_UNICODE_BIN       "/sys/unicode.bin"   /* Unicodeテーブル */
#define SYS_SQLITE_BIN        "/sys/sqlite.bin"    /* SQLite拡張域バイナリ */

/* ====================================================================== */
/*  デフォルトコマンド検索パス                                                */
/* ====================================================================== */
#define SYS_DEFAULT_PATH      "/bin:/sbin:/usr/bin"
#define SYS_DEFAULT_HOME      "/home/user"
#define SYS_DEFAULT_SHELL     "/shell"

/* ====================================================================== */
/*  RS-232C シリアル設定                                                     */
/* ====================================================================== */
#define SYS_SERIAL_BAUD       38400

/* ====================================================================== */
/*  LAN (LGY-98, docs/tasks/network/PLAN.md)                                */
/*                                                                          */
/*  BASE 0 = 無効 (既定)。有効化はビルド時: make kernel-lgy98 (戻すのは        */
/*  kernel-nolgy98)。build/config.mk がスタンプを見て CONFIG_LGY98_* を -D で   */
/*  渡す。広域スキャンや IRQ の                                              */
/*  自動検出はしない。値は PIC IRQ (3 / 5 / 6)。                             */
/* ====================================================================== */
#ifndef CONFIG_LGY98_BASE
#define CONFIG_LGY98_BASE     0
#endif
#ifndef CONFIG_LGY98_IRQ
#define CONFIG_LGY98_IRQ      0
#endif
#ifndef CONFIG_LGY98_FLAGS
#define CONFIG_LGY98_FLAGS    0       /* LGY98_FLAG_DIAG 1 | LGY98_FLAG_LOOPBACK 2 */
#endif

#endif /* CONFIG_H */
