# 05. INT 1Bh ディスク BIOS の実装計画 (Phase 3-3b)

> 作成: 2026-08-08
> 前提: [04_implementation_status.md](04_implementation_status.md) の Phase 3-3a まで完了
> 目的: **ディスクイメージからゲストをブートする** — Ys I 起動への最後の関門

---

## 1. どこまで出来ていて、何が足りないか

`INT n` を `#GP` で受けて `AH` / `CF` を書き換えてゲストに返す**器はできている**
（実機で `INT 1Bh` の往復を確認済み）。足りないのは中身だけ。

```c
/* kernel/v86_bios.c — 現状はここが空っぽ */
static void bios_int1b(u32 *frame)
{
    bios_set_ah(frame, 0x00);
    bios_set_cf(frame, 0);      /* 何もせず「成功」と嘘をついている */
}
```

やることは **PC-98 の INT 1Bh の引数を解釈して `loop_dev` を呼ぶ**、それだけ。

---

## 2. PC-98 の INT 1Bh レジスタ規約

**前回プロジェクトが取り違えて詰まった箇所**なので、ここは慎重に。
（`archive/feat-vdm` の `freedos98_boot_debug_report.md` に
「誤: `DH`=シリンダ, `CH`=ヘッド / 正: `CL`=シリンダ, `CH`=セクタ長, `DH`=ヘッド, `DL`=セクタ」
という記録が残っている）

| レジスタ | 内容 |
|---|---|
| `AH` | コマンド。bit7 が立っていると「リトライあり」等の修飾 |
| `AL` | DA/UA (装置番号)。`0x90` = 2HD FDD#1、`0x30` = 2DD、`0xA0` = SASI HDD |
| **`CL`** | **シリンダ** |
| **`CH`** | **セクタ長コード** (0=128, 1=256, 2=512, 3=1024 バイト) |
| **`DH`** | **ヘッド** |
| **`DL`** | **セクタ** (1 起算) |
| `BX` | 転送バイト数 |
| `ES:BP` | 転送バッファ |

戻り値: `CF=0` で成功、`CF=1` で失敗。`AH` にステータス。

### 実測で確認済みの呼ばれ方

Ys の IPL が実際に発行していたもの（[03_ys_profile.md](03_ys_profile.md) §3 の
シリアルトレースより）:

```
AH=D6 AL=90 CL=00 CH=03 DH=00 DL=06 BX=0400 ES=0000 BP=0600
AH=D6 AL=90 CL=00 CH=03 DH=01 DL=04 BX=1400 ES=1994 BP=0060
AH=D6 AL=90 CL=01 CH=03 DH=00 DL=01 BX=2000 ES=1994 BP=0000
```

`AH=0xD6` = READ DATA (0x06) + リトライ等の修飾ビット。
`CH=03` なので 1024 バイト/セクタ。**下位ニブルだけ見てコマンドを判定すること。**

### 最低限実装するコマンド

| AH & 0x0F | 機能 | 優先度 |
|---|---|---|
| `0x06` | READ DATA | **必須** (これが無いと何も始まらない) |
| `0x04` | SENSE (装置状態) | **必須** (DOS/ゲームが起動時に叩く) |
| `0x00` | SEEK | 高 (Ys のコピープロテクトが使う) |
| `0x03` | INITIALIZE | 中 |
| `0x05` | WRITE DATA | 中 (セーブに要る) |
| `0x07` | RECALIBRATE | 低 |

---

## 3. loop_dev への繋ぎ方

既存 API がそのまま使える。**新しいディスクコードを書く必要は無い。**

```c
int  loop_dev_attach(const char *vfs_path, int slot);
int  loop_dev_read_chs (int slot, u16 cyl, u8 head, u8 sect, void *buf);
int  loop_dev_write_chs(int slot, u16 cyl, u8 head, u8 sect, const void *buf);
int  loop_dev_get_geometry(int slot, u16 *cyls, u8 *heads, u8 *spt, ...);
u8   loop_dev_get_sec_n(int slot);      /* セクタ長コード */
int  loop_dev_get_media(int slot);      /* 2HD/2DD/2D */
void loop_dev_detach(int slot);
```

- `sect` は **1 起算** (FDC/ATA 準拠) で、INT 1Bh の `DL` と同じ。変換不要
- D88 のトラックテーブル走査は `loop_dev` が内部でやる
- **Ys のコピープロテクトにも対応済み** — `loop_dev_seek_d88()` は
  「SEEK cyl=1 → READ C=0,H=1,R=1」のような ID とトラック位置がずれた
  読み方を扱えるように作られている

### 転送先

`ES:BP` はゲストのセグメント:オフセット。バッキング RAM にリマップ済みなので、
カーネルからは `(ES << 4) + BP` をそのまま触れる（`v86_ptr()` と同じ考え方）。

**1 回の呼び出しで `BX` バイト分＝複数セクタを転送する**点に注意。
セクタ長は `CH` から得て、`DL` を増やしながら回す。
トラック境界を跨いだら `DH` → `CL` の順に繰り上げる。

---

## 4. 実装の段取り

| # | 内容 | 検証 |
|---|---|---|
| 1 | `v86_run()` にディスクイメージのパスを渡せるようにし、セッション開始時に `loop_dev_attach` | `v86 -d <image>` で attach できる |
| 2 | `bios_int1b()` に SENSE (04h) を実装 | ゲストが装置ありと判定する |
| 3 | READ DATA (06h) を実装 | ゲストが読んだ内容が期待バイトと一致する |
| 4 | IPL ロード: セッション開始前に C:0 H:0 R:1 を `0x1FC00` へ読み、`CS:IP = 1FC0:0000` で起動 | ゲストが IPL を実行し始める |
| 5 | SEEK / INITIALIZE / WRITE を追加 | Ys のプロテクトチェックが通る |

**1 と 2 だけでも「ゲストが装置を認識する」ところまで行くので、
そこで一度実機確認を挟む。** いきなり全部書かない。

---

## 5. 先に潰しておくべき懸念

### 5-1. PIC の仮想化がまだスタブ

現在 PIC ポートは「読み `0xFF` / 書き破棄」。**IPL や DOS は起動時に PIC を
初期化する**ので、そこで破綻する可能性が高い。

READ が通ってゲストが動き始めたら、次に来るのはこれ。
仮想 PIC (IMR / ISR / ICW シーケンス) の実装が Phase 3-4 になる見込み。

### 5-2. ゲストがどの装置番号で来るか

`AL` の DA/UA を見て `loop_dev` のスロットへ対応付ける必要がある。
最初は「`0x90`(2HD FDD#1) → slot 0 固定」で十分。

### 5-3. 1MB 境界

`ES:BP` が 0xFFFF:xxxx のようにラップする指定は理屈上あり得る。
バッキング RAM は 0x00000-0x8EFFF しか無いので、
**範囲外を指されたら CF=1 で返す**こと。黙って書くとカーネルを壊す。

---

## 6. 完了の目安

- `v86 -d /host/Ys.D88` で Ys の IPL が実行され、画面に何か出る
- そこまで行けば、あとは出てくる不具合を潰す作業になる

> 注意: `Ys_FM.D88` は素の NP21/W でも本編に入れない (イメージ側の問題)。
> **検証には `Ys.D88` を使うこと。**

---

## 7. 参照

- `drivers/loop_dev.h` — 使う API
- [03_ys_profile.md](03_ys_profile.md) §3 — Ys の IPL が実際に発行した INT 1Bh の値
- [04_implementation_status.md](04_implementation_status.md) §4 — 守るべき不変条件 (9 項目)
- `archive/feat-vdm:docs/tasks/v86/06_bios_emulation.md` — 前回の INT 1Bh HLE (参考)
- `git show dff9ebf~1:docs/tasks/v86/_archive/freedos98_boot_debug_report.md`
  — CHS マッピングを取り違えた記録
