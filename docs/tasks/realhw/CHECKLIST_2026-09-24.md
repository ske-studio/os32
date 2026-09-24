# 実機 (PC-9821Ra266) の回 — 2026-09-24 の手順

> 発行: PM (Claude Code `claude-opus-5-5`)。使う成果物: **CI の artifact `os32-feat-gui-c7f1ba2`** (KAPI **v63**、LZ4 高圧縮、キーボード修正、HDD 段 0、VFS / ext2 修正入り)。
> ノートの取り方: `cd ~/os32 && git pull && tools/ci_fetch.sh --sha c7f1ba2` → `./os32-ci/os32-feat-gui-c7f1ba2/`。

## やらないこと (重要)

- **HDD へのインストール (`cdinst` / `install`) はしない。** 8GB ディスクでは今のインストーラが壊す (区画の終了シリンダが 16 ビットで桁あふれ、固定の 8/17 幾何。TASK_HDD_INSTALL 段 1/2 が未実装)。
- HDD への書き込み全般 (`format`、`hdprep` は未実装)。

## 手順 (この順)

| # | 誰が | やること | 見るもの / 記録 |
|---|---|---|---|
| 1 | ユーザー | FD を書く: `os32_boot.d88` (2HD) か `os32_boot144.img` (1.44MB) | — |
| 2 | ユーザー | FD で起動、**起動画面を写真に** (スクロールするので 2〜3 枚) | `[kbd] st=.. -> .. cmd=16 flushed=..` / **`[hdd] bios da=80 cf=.. ah=.. len=.. C/H/S=.../.../...`** と `[hdd] bios da=81 ...` / **`[hdd] ata0 def=C/H/S cur=C/H/S(valid|n/a) lba=0|1 total=N`** / `CPU...` と `PIT 2.4576M` / `[pcm] CS4231 ...` / **`[selftest] N/N passed`** |
| 3 | ユーザー | **本体キーボードで打鍵** (シェルのプロンプトで `ver` と Enter など) | 文字が出るか |
| 4 | ノート | シリアルで `sync` → `cmd ver` (**API: v63**) → `cmd kbdstat` (**打鍵の前後で 2 回**) → `cmd uptime` | `kbdstat` の irq / empty / err / ovr / code / now |
| 5 | ユーザー | **桁ズレ**の切り分け: (a) ずれた画面で液晶の**自動調整**を 1 回 → 直るか、(b) ずれ方の写真 (古い行も一緒に傾くか、新しい行だけか)、(c) `gfxmode pc98` → リセット → ずれが消えるか | 直る / 変わらない |
| 6 | ノート | `cmd timetest` (W4 の CPL=3、v63 の再確認) | `TIME PASS` |
| 7 | 任意 | PCM の音 (E6): `cmd "pcmtest short"` | 1kHz の音が出るか (耳) |

## キーボードが効かなかったら (`kbdstat` の読み方、POLICY_DEBUG §4-57)

| kbdstat | 疑う場所 |
|---|---|
| irq = 0、now の bit1 (RxRDY) = 1 | 割り込み経路 (PIC / IRQ1) |
| irq = 0、RxRDY = 0 | キーボードが送っていない (RTY# / RDY# / 配線) |
| irq > 0 なのに文字が出ない | 配送側 (リング / rshell) |
| 同じ code で irq が暴走 | 再送ストーム |

## 持ち帰るもの

写真 (起動画面 2〜3 枚、桁ズレ)、ノートのシリアルの出力。**`[hdd]` の 3 行で TASK_HDD_INSTALL 段 1 の設計値 (BIOS 幾何・LBA の可否) が決まる。**
