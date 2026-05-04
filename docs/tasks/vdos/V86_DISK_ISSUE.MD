# V86 FDD バグ修正計画書

VDOS (V86モード) における**実FDD非動作**および**2DDメディア非対応**問題の修正計画。
本書は単独で読めるよう、対象コードの該当箇所をすべてファイル:行番号付きで記載する。

## 0. 前提コンテキスト

### 0.1 関連ファイル
| ファイル | 役割 |
|---|---|
| `kernel/v86_session.c` | V86セッションのライフサイクル。`v86_boot_freedos()` (FDIファイル) / `v86_boot_physical_fdd()` (実FDD) のエントリ |
| `kernel/v86_disk.c` | INT 1Bh ディスクBIOSの仮想化。FDIイメージモードと実FDDモードを切替 |
| `kernel/v86_disk.h` | FDDジオメトリ定数・APIプロトタイプ |
| `drivers/fdc.c` | µPD765A FDCの直接制御 (リセット/シーク/READ/WRITE) |
| `drivers/fdc.h` | FDC I/Oポート定数・コマンド定数・ジオメトリ定数 |
| `kernel/kernel.c` | ブート時 `fdc_init()` 呼び出し |
| `kernel/isr_handlers.c` | IRQ11 ハンドラ (`fdc_irq_handler`) |
| `programs/cmds/vdos.c` | ユーザーランド `vdos` コマンド (引数なしで実FDDモード) |
| `kapi/kapi_generated.c` | `sys_v86_boot_physical` ブリッジ |

### 0.2 PC-9801 FDDインターフェース概要 (PC9800Bible §4-3 §17, §25, §26)

| インターフェース | I/Oポート | IRQ | DMA | DA/UA |
|---|---|---|---|---|
| 1MB FDC (765相当) | 0x90/0x92/0x94 | IR11 (slave) | ch2 | 0x90-0x93 |
| 640KB FDC (765相当) | 0xC8/0xCA/0xCC | IR2 (master) | ch3 | 0x70-0x73 |
| 1MB/640KB 切換I/F | **0xBE** (FDE/PE bit) | — | — | — |
| 1.44MB両用 | 0x90系 + 切換 | IR11 | ch2 | 0x30-0x33, 0xB0-0xB3 |

**重要**: 現在のドライバ (`drivers/fdc.c`) は 1MB FDC (0x90/0x92/0x94) のみ実装。640KB専用FDC(0xC8系)は未対応。
→ 本計画では **1MB/640KB両用ドライブ (内蔵FDD)** に媒体として 2DD を挿入したケースのみを扱う。
   - 切換ポート 0xBE で 1MBモード→1MBモードのまま、媒体を 2DD として読む = DA/UA 0x10 (両用1MBモードの640KBフォーマット)
   - もしくは 0xBE で 640KBモードに切替 = DA/UA 0x70 でアクセス
   - **本計画では DA/UA 0x10 経路を選択** (NP21/W 標準動作と整合的なため)

### 0.3 INT 1Bh DA/UA 一覧 (PC9800Bible §2-9 表2-34)

| DA/UA | 媒体 | フォーマット | N | C × H × S |
|---|---|---|---|---|
| 0x90-93 | 2HD (1MB) | 1024B/sec MFM | 3 | 77 × 2 × 8 |
| 0x10-13 | 2DD (1MB/640KB両用1MBモード) | 512B/sec MFM | 2 | 80 × 2 × 8 *or* 9 |
| 0x70-73 | 2DD (640KB専用 or 両用640KBモード) | 512B/sec MFM | 2 | 80 × 2 × 8 *or* 9 |
| 0x30-33 | 2HD (1.44MB) | 512B/sec MFM | 2 | 80 × 2 × 18 |

---

## 1. 問題1: 実FDDが使えない

### 1.1 症状
`vdos` を引数なしで起動 → "VDOS failed to start" または IPL読み込みでハング。

### 1.2 呼び出し経路 (確認済み・配線は正しい)
```
programs/cmds/vdos.c:36   sys_v86_boot_physical(0, cmdline)
└→ kapi/kapi_generated.c:795  v86_boot_physical_fdd(0, cmdline)
   └→ kernel/v86_session.c:412  v86_boot_physical_fdd()
      ├─ v86_disk_set_physical(0)        // v86_session.c:431
      ├─ fdc_read_sector(0,0,0,1, ipl)  // v86_session.c:435  ★ここで失敗
      └─ v86_session_run_core()         // v86_session.c:445
```
- `fdc_init()` はブート時に `kernel/kernel.c:158-167` で呼ばれる
- IRQ11 ハンドラは `kernel/isr_handlers.c:394` (`fdc_irq_handler`) → `fdc_irq_fired = 1` で正しく接続済み
- IDT エントリ: `kernel/idt.c` で IRQ11 → INT 0x2B
- → **配線(IRQ/IDT/PIC)は正常**。失敗するのは FDC の状態管理。

### 1.3 根本原因 (3つの複合要因)

#### 原因A: ブート時1回のみ初期化 (`drivers/fdc.c:373-394`, `kernel/kernel.c:161`)

`fdc_init()` は `fdc_reset() → fdc_recalibrate(0)` を実行する。
`fdc_recalibrate()` (`drivers/fdc.c:224-245`) は **シリンダ0へのSEEK完了IRQ待ち**を行うが、
ブート時に FDD にメディアが入っていない場合、

1. `fdc_send_byte(FDC_CMD_RECALIBRATE)` (line 229) は通る
2. `fdc_wait_irq(300)` (line 233) が3秒タイムアウト → `return -2`
3. `kernel/kernel.c:165` が "ER" を表示してそのまま続行

このあと**ユーザーが NP21/W に FDDイメージをマウントしても、再 init は走らない**。
FDC の内部状態は「最後のリキャリブレートが失敗した状態」のまま放置される。

#### 原因B: SEEK実行前にメディア挿入チェックなし (`drivers/fdc.c:250-275`)

`fdc_seek()` も同じく IRQ待ちでタイムアウトする。`fdc_read_sector()` のリトライループ
(`drivers/fdc.c:286-318`) は3回試すが、毎回 SEEK タイムアウト → READ コマンド未送信 →
リザルト読み出しもされない。

#### 原因C: NP21/W のセクタ未準備状態 (`v86_session.c:435`)

ユーザーがマウント直後に `vdos` 実行 → エミュレータ側のセクタイメージが
「FDC リセット直後で先頭セクタ未到達」状態のことがある。リトライ・ウェイトが無い。

### 1.4 修正方針

**Phase 1A**: `v86_boot_physical_fdd()` の冒頭で `fdc_init()` を再実行する。
**Phase 1B**: IPL 読み込みをリトライ可能にする。
**Phase 1C** (任意): モーター起動の冪等性を確保。

### 1.5 具体的な変更

#### 1.5.1 `kernel/v86_session.c:412-448` を変更

**変更前** (line 412-448 抜粋):
```c
int v86_boot_physical_fdd(int drv, const char *cmdline)
{
    u8 *ipl_dst;

    /* セッション初期化 */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;
    current_session.auto_delay_remaining = V86_AUTO_TYPE_DELAY;
    current_session.fd = -1;
    current_session.img_data_size = V86_FDD_IMAGE_SIZE;

    /* V86メモリ空間を構築 */
    v86_mem_setup();

    /* PIC/PIT初期化 */
    v86_pic_init();
    v86_pit_init();

    /* 実FDDモードを設定 */
    v86_disk_set_physical(drv);

    /* IPLを実FDCから読み込み (シリンダ0, ヘッド0, セクタ1) */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    if (fdc_read_sector(drv, 0, 0, 1, ipl_dst) != 0) {
        kprintf(0xE1, "[V86] FDC read IPL failed (drv=%d)\n", drv);
        v86_disk_clear();
        v86_mem_teardown();
        return -1;
    }
    ...
}
```

**変更後** (★が新規):
```c
int v86_boot_physical_fdd(int drv, const char *cmdline)
{
    u8 *ipl_dst;
    int retry;
    int ipl_rc = -1;

    /* セッション初期化 */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;
    current_session.auto_delay_remaining = V86_AUTO_TYPE_DELAY;
    current_session.fd = -1;
    current_session.img_data_size = V86_FDD_IMAGE_SIZE;

    /* ★Phase 1A: FDCを再初期化 (ブート時に失敗していたケースの救済)
     *  メディア挿入後にユーザーが vdos を起動するため、毎回 reset+specify+recalibrate
     *  を再実行する必要がある。 */
    {
        int init_rc = fdc_init();
        if (init_rc != 0) {
            kprintf(0xE1, "[V86] fdc_init failed (rc=%d). Insert FDD media and retry.\n",
                    init_rc);
            return -1;
        }
    }

    /* V86メモリ空間を構築 */
    v86_mem_setup();

    /* PIC/PIT初期化 */
    v86_pic_init();
    v86_pit_init();

    /* 実FDDモードを設定 */
    v86_disk_set_physical(drv);

    /* ★Phase 1B: IPLを実FDCから読み込み (3回リトライ + 各リトライ前に再recalibrate) */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    for (retry = 0; retry < 3; retry++) {
        ipl_rc = fdc_read_sector(drv, 0, 0, 1, ipl_dst);
        if (ipl_rc == 0) break;
        kprintf(0xA1, "[V86] IPL read retry %d/3 (rc=%d)\n", retry + 1, ipl_rc);
        /* 失敗時: FDCをreset して次のリトライに備える */
        if (fdc_init() != 0) {
            /* それでも駄目なら諦める */
            break;
        }
    }
    if (ipl_rc != 0) {
        kprintf(0xE1, "[V86] FDC read IPL failed after 3 retries (drv=%d)\n", drv);
        v86_disk_clear();
        v86_mem_teardown();
        return -1;
    }

    kprintf(0xA1, "[V86] Booting from physical FDD (drv=%d)...\n", drv);
    v86_session_run_core();
    return 0;
}
```

#### 1.5.2 `drivers/fdc.c:373` の `fdc_init()` を冪等化

`fdc_init()` を複数回呼ばれる前提で書き直す:

**変更前** (line 373-394):
```c
int fdc_init(void)
{
    int ret;
    fdc_motor_on();
    ret = fdc_reset();
    if (ret != 0) return ret;
    ret = fdc_recalibrate(0);
    if (ret != 0) {
        ret = fdc_recalibrate(0);
    }
    fdc_recalibrate(1);
    return ret;
}
```

**変更後**:
```c
int fdc_init(void)
{
    int ret;

    /* IRQフラグを確実にクリア (前回の取りこぼし対策) */
    fdc_irq_fired = 0;

    /* モーターON (既にONなら待たない) */
    fdc_motor_on();

    /* リセット + Specify */
    ret = fdc_reset();
    if (ret != 0) return ret;

    /* Recalibrate (ヘッドをシリンダ0に移動)
     * 1回目失敗 → リセット直後で安定化前の可能性。100ms 待ってもう一度。 */
    ret = fdc_recalibrate(0);
    if (ret != 0) {
        u32 start = tick_count;
        while ((tick_count - start) < 10) { /* 100ms ウェイト */ }
        ret = fdc_recalibrate(0);
    }

    /* drive 1 (内蔵2台目) は接続されていないとタイムアウトするので
     * エラーは無視する */
    fdc_recalibrate(1);

    return ret;
}
```

#### 1.5.3 `drivers/fdc.c:164-172` `fdc_motor_on()` を冪等化

**変更前**:
```c
static void fdc_motor_on(void)
{
    outp(FDC_CTRL, CTRL_MTON | CTRL_DMAE);
    /* モータースピンアップ待ち: 約300ms */
    {
        u32 start = tick_count;
        while ((tick_count - start) < 30) { /* 何もしない */ }
    }
}
```

**変更後** (既ON判定を追加):
```c
static void fdc_motor_on(void)
{
    /* リードスイッチ (0x94 R) で現在の MTON 状態を確認可能だが、
     * NP21/W では 0x94 リードがリードスイッチを返すため判別不能。
     * → 静的フラグで前回の状態を覚える */
    static int motor_running = 0;

    outp(FDC_CTRL, CTRL_MTON | CTRL_DMAE);
    if (!motor_running) {
        /* 初回のみ300msスピンアップ待ち */
        u32 start = tick_count;
        while ((tick_count - start) < 30) { /* 何もしない */ }
        motor_running = 1;
    }
}
```

(`fdc_motor_off` は使用箇所なし。手をつけない)

### 1.6 検証手順

1. `bash build.sh` でビルドエラー0を確認
2. `bash build_fat.sh` でフロッピーイメージ生成
3. NP21/W 起動 → OS32 ブート → 起動時 FDC = "ER" でも続行
4. NP21/W で 2HD イメージを FDD#0 にマウント
5. `vdos` 実行 (引数なし)
6. **期待動作**: "Booting from physical FDD..." 表示後、FreeDOS の IPL ロゴが出る
7. リトライログが出るかも (`IPL read retry 1/3` 等) — それでも最終的に成功すればOK

### 1.7 受け入れ条件
- [ ] 起動時にFDD未マウントでも、後から `vdos` で実FDDブート可能
- [ ] 2HD FreeDOS(98) ディスクから FreeDOS が起動する
- [ ] 既存の `vdos <fdi_path>` (イメージモード) に回帰がない
- [ ] `bash build.sh` でエラー0

### 1.8 リスク・注意点
- `fdc_init()` 呼び出しは V86 開始**前**でなければならない (V86中はRing 0 IRQが届くが、状態が複雑になる)
- `fdc_motor_on()` の冪等化は static 変数を使う。**リセット直後は強制的にONとして書き直すべき** → `fdc_reset` 内で `motor_running = 0` に戻すパスは不要 (リセットは0x94への CTRL_RST 単独書き込みで MTON を落とすため、`fdc_motor_on` 直後の `outp(FDC_CTRL, CTRL_MTON | CTRL_DMAE)` で再アサート済み)
- `tick_count` は `extern volatile u32` (`kernel/idt.c`)。`drivers/fdc.c:21` で既に extern 宣言済み

---

## 2. 問題2: 2DD ディスクの非対応

### 2.1 症状
2DD (640KB) フロッピーをマウントして `vdos` 実行 → INT 1Bh が `0x40` (DA/UA不適当) または `0xC0` (セクタ未検出) を返す → DOS のディスク読み出しが失敗。

### 2.2 設計上の制約 (`docs/V86_DESIGN_ISSUES.md:235-250` で既に列挙)

| 層 | ファイル:行 | ハードコード内容 |
|---|---|---|
| INT 1Bh DA/UA | `kernel/v86_disk.c:125` | `(daua & 0xF0) == 0x90` のみ受理 |
| ジオメトリ定数 | `kernel/v86_disk.h:14-19` | `77×2×8×1024` 固定 |
| セクタ長コードチェック | `kernel/v86_disk.c:226, 352` | `sector_len != 3` を 0xC0 で拒絶 |
| READ ID 戻り値 | `kernel/v86_disk.c:484-487` | `CH=N=3` 固定 |
| BDA結果バッファ N値 | `kernel/v86_disk.c:327, 446` | `sector_len` をそのまま記録 (これは正しい) |
| FDCドライバ | `drivers/fdc.h:83-91` | `FDC_SECTOR_SIZE=1024, FDC_SECTOR_N=3` |
| FDC READ コマンド | `drivers/fdc.c:300, 349` | `FDC_SECTOR_N` を直送 |
| FDC GAP3 | `drivers/fdc.c:88` | `0x74` (1024B/sec MFM 用) |
| データレートCCR | `drivers/fdc.c` 全体 | **未実装** (PC-98ではモード切替で対応) |
| 切換ポート 0xBE | `drivers/fdc.c` 全体 | **未実装** |

### 2.3 修正方針

ジオメトリ抽象化を導入し、**メディア種別ごとのパラメータ構造体**を持ち回す。

#### 2.3.1 サポートする2DDフォーマット
本計画ではDOS実用上重要な以下のみを対象:
- **2DD-8**: 80 cyl × 2 head × 8 sec × 512B (640KB, N=2) — DA/UA 0x10
- **2DD-9**: 80 cyl × 2 head × 9 sec × 512B (720KB, N=2) — DA/UA 0x10

**範囲外**: 1.44MB (0x30系)、320KB (0x50系)、640KB専用FDC (0xC8系)。これらは将来の課題とする。

### 2.4 具体的な変更

#### 2.4.1 `drivers/fdc.h` にジオメトリ構造体を追加

**変更前** (line 81-91):
```c
#define FDC_CYLINDERS    77
#define FDC_HEADS        2
#define FDC_SPT          8
#define FDC_SECTOR_SIZE  1024
#define FDC_SECTOR_N     3
#define FDC_GAP3         0x74
#define FDC_TOTAL_SECTORS (FDC_CYLINDERS * FDC_HEADS * FDC_SPT)
#define FDC_DATARATE     0
#define FDC_TIMEOUT_LOOP 10000
```

**変更後**:
```c
/* メディア種別 */
typedef enum {
    FDC_MEDIA_2HD_1232 = 0,  /* 1.2MB (PC-98標準, 77×2×8×1024) */
    FDC_MEDIA_2DD_640  = 1,  /* 640KB (80×2×8×512) */
    FDC_MEDIA_2DD_720  = 2   /* 720KB (80×2×9×512) */
} fdc_media_t;

/* メディアジオメトリ構造体 */
struct fdc_geom {
    u8  cyls;       /* シリンダ数 */
    u8  heads;      /* ヘッド数 */
    u8  spt;        /* セクタ/トラック */
    u8  sec_n;      /* セクタ長コード (2=512B, 3=1024B) */
    u16 bps;        /* バイト/セクタ */
    u8  gap3;       /* GAP3長 (R/W用) */
    u8  daua_high;  /* DA/UA上位ニブル (0x90/0x10/0x70) */
};

/* 既知メディア定義 (drivers/fdc.c で実体化) */
extern const struct fdc_geom fdc_geom_2hd;
extern const struct fdc_geom fdc_geom_2dd_640;
extern const struct fdc_geom fdc_geom_2dd_720;

/* デフォルト = 2HD (互換) */
#define FDC_CYLINDERS    77
#define FDC_HEADS        2
#define FDC_SPT          8
#define FDC_SECTOR_SIZE  1024
#define FDC_SECTOR_N     3
#define FDC_GAP3         0x74
#define FDC_TOTAL_SECTORS (FDC_CYLINDERS * FDC_HEADS * FDC_SPT)
#define FDC_TIMEOUT_LOOP 10000
```

`FDC_DATARATE` 定数は **削除** (未使用かつ PC-98 では 0xBE 経由で切り替え)。

#### 2.4.2 `drivers/fdc.c` にジオメトリ実体と新APIを追加

**追加**(末尾の `fdc_init()` 直前あたり):
```c
const struct fdc_geom fdc_geom_2hd = {
    77, 2, 8, 3, 1024, 0x74, 0x90
};
const struct fdc_geom fdc_geom_2dd_640 = {
    80, 2, 8, 2, 512, 0x2A, 0x10  /* GAP3=0x2A は MFM 512B/sec の標準値 */
};
const struct fdc_geom fdc_geom_2dd_720 = {
    80, 2, 9, 2, 512, 0x2A, 0x10
};
```

`fdc_read_sector` / `fdc_write_sector` を**ジオメトリ受け取り版に拡張**。
**既存シグネチャは互換のため残す**(2HD固定で内部委譲):

```c
/* 新API: ジオメトリ指定版 */
int fdc_read_sector_geom(int drv, int cyl, int head, int sect,
                         const struct fdc_geom *g, void *buf);
int fdc_write_sector_geom(int drv, int cyl, int head, int sect,
                          const struct fdc_geom *g, const void *buf);

/* 既存API: 2HD固定 */
int fdc_read_sector(int drv, int cyl, int head, int sect, void *buf)
{
    return fdc_read_sector_geom(drv, cyl, head, sect, &fdc_geom_2hd, buf);
}
int fdc_write_sector(int drv, int cyl, int head, int sect, const void *buf)
{
    return fdc_write_sector_geom(drv, cyl, head, sect, &fdc_geom_2hd, buf);
}
```

`fdc_read_sector_geom` の実装は既存 `fdc_read_sector` (drivers/fdc.c:280-321) を
ベースに以下を変更:
- `FDC_SECTOR_SIZE` → `g->bps`
- `FDC_SECTOR_N` → `g->sec_n`
- `FDC_GAP3` → `g->gap3`
- DMAバッファサイズが固定 `FDC_SECTOR_SIZE=1024` のため、現状の static `dma_buffer[1024]` は2DD(512B)でも流用可能 (使用領域は先頭512Bのみ)

**0xBE 切換は本フェーズでは省略**: NP21/W は DA/UA 0x10 で 1MBモードのまま 2DD を読める動作をする。実機検証時に必要なら別タスクで追加。

#### 2.4.3 `kernel/v86_disk.h` にメディア指定APIを追加

**変更前** (line 27-34):
```c
void v86_disk_set_file(int fd, u32 data_offset, u32 data_size);
void v86_disk_clear(void);
void v86_disk_set_physical(int drv);
```

**変更後**:
```c
#include "fdc.h"  /* fdc_media_t, struct fdc_geom */

void v86_disk_set_file(int fd, u32 data_offset, u32 data_size,
                       fdc_media_t media);
void v86_disk_clear(void);
void v86_disk_set_physical(int drv, fdc_media_t media);

/* メディア種別→ジオメトリ取得 */
const struct fdc_geom *v86_disk_get_geom(void);
```

ジオメトリ定数 `V86_FDD_*` (line 14-19) は削除し、動的ジオメトリ参照に置換。

#### 2.4.4 `kernel/v86_disk.c` の DA/UA・ジオメトリチェックを動的化

**変更前** (line 124-130):
```c
if ((daua & 0xF0) == 0x90) {
    /* 1MB FDD */
} else {
    DISK_ERROR_RETURN(log_entry, 0x40, -1, regs);
}
```

**変更後**:
```c
{
    const struct fdc_geom *g = v86_disk_get_geom();
    /* 現在マウント中のメディアの DA/UA 上位ニブルと一致するか確認 */
    if ((daua & 0xF0) != g->daua_high) {
        DISK_ERROR_RETURN(log_entry, 0x40, -1, regs);
    }
}
```

**変更前** (line 226):
```c
if (sector_len != 3) {
    DISK_ERROR_RETURN(log_entry, 0xC0, -2, regs);
}
```

**変更後**:
```c
{
    const struct fdc_geom *g = v86_disk_get_geom();
    if (sector_len != g->sec_n) {
        DISK_ERROR_RETURN(log_entry, 0xC0, -2, regs);
    }
}
```

CHS→オフセット計算 (line 246-248):
```c
byte_offset = ((u32)cylinder * g->heads + (u32)head_dh)
              * ((u32)g->spt * g->bps)
              + (u32)sector_dl * g->bps;
```

範囲チェック (line 242):
```c
if (cylinder >= g->cyls || head_dh >= g->heads) { ... }
```

`fdc_read_sector` 呼び出しを `fdc_read_sector_geom` に変更し、ジオメトリ pointer を渡す。
WRITE 側 (`case 0x05:` line 339-449) も同様に動的化。

READ ID (case 0x0A, line 479-491) も `g->sec_n` を返す:
```c
regs[V86_REG_ECX] = (u32)cylinder_cl
                  | ((u32)g->sec_n << 8);
```

#### 2.4.5 `kernel/v86_session.c` のセッションエントリにメディア指定追加

##### `v86_boot_freedos()` (line 346-404)
FDIヘッダ判定後、データサイズからメディア種別を推定:
```c
/* ジオメトリ推定: データサイズで判定 */
fdc_media_t media;
u32 dsz = current_session.img_data_size;
if (dsz == 1261568) media = FDC_MEDIA_2HD_1232;       /* 77×2×8×1024 */
else if (dsz == 655360) media = FDC_MEDIA_2DD_640;    /* 80×2×8×512  */
else if (dsz == 737280) media = FDC_MEDIA_2DD_720;    /* 80×2×9×512  */
else {
    kprintf(0xE1, "[V86] Unknown FDI image size: %u bytes\n", dsz);
    vfs_close(fd);
    return -1;
}

v86_disk_set_file(fd, current_session.img_offset,
                  current_session.img_data_size, media);
```

##### `v86_boot_physical_fdd()` (line 412-448)
将来の自動判別までは引数で受ける、もしくは固定で 2HD を仮定:
```c
int v86_boot_physical_fdd(int drv, const char *cmdline)
```
↓
```c
int v86_boot_physical_fdd(int drv, fdc_media_t media, const char *cmdline)
```

KAPI シグネチャも更新が必要 (後述 §2.4.7)。

実FDDからのIPL読み込み:
```c
const struct fdc_geom *g = v86_disk_get_geom();
ipl_dst = v86_phys_addr(IPL_SEG, 0);
for (retry = 0; retry < 3; retry++) {
    ipl_rc = fdc_read_sector_geom(drv, 0, 0, 1, g, ipl_dst);
    if (ipl_rc == 0) break;
    ...
}
```

#### 2.4.6 自動メディア判別 (実FDD用)

実FDDでメディア種別を自動判別する手段:
1. FDC `READ ID` コマンド (0x0A) を物理FDC に発行 → 返ってくる N 値 を見る
2. 1MB系FDC (0x90) で N=3 が返れば 2HD、N=2 が返れば 2DD と判別

`drivers/fdc.c` に `fdc_probe_media()` を新設:
```c
int fdc_probe_media(int drv, fdc_media_t *media);
```
内部実装は `FDC_CMD_READ_ID` を発行して N をリザルトから取得。
2DD 時は内蔵FDD両用モードで N=2、9セクタフォーマットか8セクタかは
追加で `READ ID` を別位置で叩くか、最後の `READ DATA` 結果で判定。

**簡易版**: 当面は `vdos` コマンドにオプション `-2dd` を追加し、ユーザー指定とする:
```
vdos             → 実FDD 2HD として読む (デフォルト)
vdos -2dd        → 実FDD 2DD として読む
vdos foo.fdi     → イメージ (サイズで自動判別)
```

#### 2.4.7 KAPI 拡張

`exec/exec.h` の `KernelAPI` 構造体に、既存 `sys_v86_boot_physical` を残しつつ、
2DD対応版を**末尾に追加** (バイナリ互換のため):

```c
struct KernelAPI {
    ...既存フィールド...
    /* idx 154 */ int (*sys_v86_boot_physical)(int drv, const char *cmdline);
    ...
    /* 末尾に追加 */
    int (*sys_v86_boot_physical_ex)(int drv, int media, const char *cmdline);
};
```

`KAPI_VERSION` をインクリメント、`KAPI_SPEC.md` 更新、`kapi/kapi_v86.c` (or 該当する
`kapi_*.c`) に実装追加。

`programs/cmds/vdos.c` は `sys_v86_boot_physical_ex` を使うように更新。

### 2.5 検証手順

1. `bash build.sh` エラー0
2. `bash build_programs.sh` エラー0
3. **2HD回帰**: 既存の 2HD FreeDOS イメージで `vdos foo.fdi` が動く
4. **2DD イメージ**: 640KB ext2 内 FDI を `vdos disk2dd.fdi` で起動 → DOS が立ち上がる
5. **2DD 実FDD**: NP21/W に 2DD イメージをマウント → `vdos -2dd` で起動
6. INT 1Bh ログ (`v86_disk_dump_log()`) で N=2 の SENSE/READ が成功していることを確認

### 2.6 受け入れ条件
- [ ] 2HD 動作に回帰なし (DA/UA 0x90, N=3)
- [ ] 2DD イメージファイル(640KB / 720KB) が起動
- [ ] 2DD 実FDD が `-2dd` オプション付きで起動
- [ ] `KAPI_VERSION` インクリメント済み、`KAPI_SPEC.md` 更新済み
- [ ] `docs/V86_DESIGN_ISSUES.md:235-250` の該当項目を ✅ に更新

### 2.7 リスク・注意点

#### 2.7.1 DMAバッファ
`drivers/fdc.c:28` の `static u8 dma_buffer[FDC_SECTOR_SIZE]` は1024B固定。
**2DD(512B) は使用領域が先頭512Bだけになる** ため動作上は問題ないが、
将来 1.44MB (N=2, 18 sec/track) の複数セクタ転送に拡張する際は再検討要。

#### 2.7.2 GAP3 値
2DD MFM 512B/sec の標準 GAP3 は 0x2A。これは µPD765A データシート由来の値。
NP21/W が GAP3 を厳密チェックしているかは不明だが、実機との互換性のため正値を使う。

#### 2.7.3 FDIヘッダの種別フィールド
FDI ヘッダ (offset 0x08-0x0B = ヘッダサイズ、0x10-0x13 = メディアタイプ) には
メディアタイプフィールドがある。
- `0x00` = 2HD/1232KB
- `0x01` = 2HDD/1.44MB
- `0x10` = 2DD/640KB
- `0x90` = 2HC/1.2MB

サイズ判定の代わりにヘッダのメディアタイプを参照する方が堅牢。
ただし純IMG (ヘッダなし) はサイズ判定にフォールバック。

`v86_session.c:366-378` の FDI 判定ロジックを以下に拡張:
```c
{
    u8 hdr[16];
    vfs_read_fd(fd, hdr, 16);
    if (current_session.img_data_size > 0x1000) {
        u32 hdr_size = *(u32 *)(hdr + 8);
        if (hdr_size == 0x1000 || hdr_size == 0x2000) {
            current_session.img_offset = hdr_size;
            current_session.img_data_size -= hdr_size;
            /* メディアタイプフィールド (offset 0x10) 参照 */
            switch (hdr[16 - 6]) { /* TODO: 正しいオフセット確認 */
                case 0x10: media = FDC_MEDIA_2DD_640; break;
                case 0x90: media = FDC_MEDIA_2HD_1232; break;
                ...
            }
        }
    }
}
```
※ FDI フォーマット仕様は `docs/PC9800Bible/` または NP21/W ソース要参照。

#### 2.7.4 9セクタ vs 8セクタの2DD自動判別
イメージファイル: サイズで判別 (655360 = 8sec, 737280 = 9sec)
実FDD: READ ID では SPT は判らない。SENSE→トラック0読み出しでブートセクタの
BPB (BIOS Parameter Block) 内 sectors-per-track を見る…が IPL 読み込み前にこれは無理。
→ **当面は `-2dd-9` オプションで明示指定** とし、9セクタは将来課題とする。

---

## 3. 作業順序とPR分割案

### PR 1: 実FDD修正 (Phase 1A/1B/1C)
- 対象: `kernel/v86_session.c:412-448`, `drivers/fdc.c:164-172, 373-394`
- 工数: 2-4時間
- 規模: 約 50-80 行
- リスク: 低 (既存2HDパスのみ)
- これだけで実FDDブートが復活する

### PR 2: ジオメトリ抽象化 (Phase 2 基盤)
- 対象: `drivers/fdc.h`, `drivers/fdc.c`, `kernel/v86_disk.h/c`
- 内容: `struct fdc_geom`, `fdc_read_sector_geom`, `v86_disk_set_*` シグネチャ変更
- 既存呼び出しはラッパで2HD固定 → 動作回帰なし
- 工数: 4-6時間
- 規模: 約 200 行 (主に置換)

### PR 3: KAPI 拡張 + vdos UI (Phase 2 統合)
- 対象: `exec/exec.h`, `kapi/kapi_v86.c`, `programs/cmds/vdos.c`, `KAPI_SPEC.md`
- 内容: `sys_v86_boot_physical_ex` 追加、`-2dd` オプション
- 工数: 2-3時間

### PR 4: FDI ヘッダメディア種別判定
- 対象: `kernel/v86_session.c:366-378`
- 工数: 2時間
- 必要に応じて

合計工数感: **1-2人日**

---

## 4. テスト用アセット

### 4.1 必要なテストFDDイメージ
| ファイル | サイズ | 用途 |
|---|---|---|
| `tests/freedos_2hd.fdi` | 1262592 | 既存 2HD FreeDOS (回帰テスト) |
| `tests/freedos_2dd_640.fdi` | 663552 | 2DD 640KB FreeDOS (新規入手必要) |
| `tests/freedos_2dd_720.fdi` | 745472 | 2DD 720KB FreeDOS (新規入手必要、優先度低) |

### 4.2 NP21/W設定
- BIOS: ITF or PC-9801 BIOS
- FDD#0: テスト対象イメージ
- FDD#1: 空 (`fdc_recalibrate(1)` のタイムアウト確認用)

---

## 5. 既知の積み残し (本計画では扱わない)

- 1.44MB (DA/UA 0x30) 対応
- 320KB (DA/UA 0x50) 対応
- 640KB専用FDC (port 0xC8系, IRQ2) 対応
- 0xBE 切換ポートの能動操作 (両用FDDでの640KBモード切替)
- A20 / HMA 対応 (`v86_mem.c` `v86_phys_addr` の1MBラップアラウンド)
- INT 1Bh 機能 0Eh (FORMAT)、09h (WRITE ID) 実装
- ライトプロテクト検出 (常に「無し」固定)
- 9セクタ2DDの実FDD自動判別

---

## 6. 参考情報

| 文書 | 該当節 |
|---|---|
| `docs/PC9800Bible/2-9_ディスク.md` | DA/UA表 (§2-9-2 表2-34)、INT 1Bh 仕様 (§2-9-3) |
| `docs/PC9800Bible/4-3_I_Oマップ.md` | §17 (1MB FDC), §25 (1MB/640KB切換), §26 (640KB FDC) |
| `docs/PC9800Bible/1-4_割り込み.md` | IR2 / IR11 割り込みレベル |
| `docs/PC9800Bible/1-5_DMA.md` | DMA ch2/ch3 の使い分け |
| `docs/V86_DESIGN_ISSUES.md` | §6 (ディスク仮想化の制約) |
| µPD765A データシート | コマンド・ステータス・GAP3標準値 |
| NP21/W ソース `np2/io/fdc.c` | エミュレータ側の挙動参考 |

---

## 7. 計画書バージョン

| Ver | 日付 | 変更内容 |
|---|---|---|
| 1.0 | 2026-05-04 | 初版 (Phase 1: 実FDD修正 / Phase 2: 2DD対応) |
