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

## 追補 (同日午後): 新しい FD `os32-feat-gui-8d17716`

午前の回の結果: キーボードは実機で入力できた (0x16 修正の合格)。`[hdd]` は BIOS / ATA とも 16382/16/63・LBA 可・total 16514063。`gfxmode pc98` で桁ズレが消えた。シリアルは 09-23 17:08 から無応答 (同じビルドで 16:38 には通っていた、ソフト要因は見つからず)。

新しい FD (CI の 8d17716) で見るもの:

| # | 見るもの | 期待 |
|---|---|---|
| 1 | 起動画面の `[pegc] hsync=24k|31k 09a8=.. bios054c.b5=. bios0459.b0=.` | 値を写真に (31k なら桁ズレの見立てどおり、24k なら次の候補 = PEGC probe の 6Ah 21h→20h) |
| 2 | `gfxmode auto` に戻してリセット → シェルの桁ズレ | 消えていれば合格 |
| 3 | IRQ3 の 3 行 | 消える。代わりに `irq: IRQ3 pending in IRR -> unmask part skipped 29 checks` が出うる (失敗ではない) |
| 4 | `FONT..` | OK (FD にも既定フォントが入った、`/sys/font/default.kcg`) |
| 5 | FD の中身 | CD の Minimal と同じ。試験用 `timetest` / `pcmtest` は FD から外れた (CD の DEBUG にある) |

シリアルの切り分け (本体キーボードで):
1. ノートから `ver` を数回送った後の画面: `> ver` が出る → 受信は正常 (送信側を疑う) / 化けた `> ...` → 速度 / 何も出ない → 受信が届いていない。
2. ESC → `serial` (状態) → `terminal` (双方向に 1 文字ずつ)。
3. `serial 9600` → `rshell` → ノートから再送。
4. 駄目なら電源を切って入れ直す (リセットでなく)。ノート側は `fuser /dev/ttyUSB0`、`stty -F /dev/ttyUSB0 -a` の crtscts。
