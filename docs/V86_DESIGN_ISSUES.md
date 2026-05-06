# V86 (Virtual 8086) サブシステム — 設計問題点 / 未実装一覧

本書は OS32 の V86 (VDOS) サブシステムの設計をデバッグ／実装担当 AI が
読める形に整理したリファレンスである。「現状動いている振る舞い」よりも
「未実装」「スタブ」「設計上の不整合」「危険な前提」を優先して列挙する。

---

## 0. ソースファイル所在マップ

| ファイル | 役割 | 主な公開 API |
|----------|------|--------------|
| `kernel/v86.h` | V86 共通ヘッダ／統計カウンタ宣言 | `v86_enter`, `v86_gp_handler`, `v86_inject_timer_irq` |
| `kernel/v86.c` | #GP デコーダ ＆ IRQ 注入 | `v86_gp_handler`, `v86_inject_timer_irq` |
| `kernel/v86_entry.asm` | V86 への IRETD 遷移 | `v86_enter` |
| `kernel/v86_mem.[ch]` | バッキング RAM・ページテーブル・IVT・BDA・I/O ビットマップ | `v86_mem_setup/_teardown`, `v86_phys_addr`, `v86_restore_screen` |
| `kernel/v86_pic.[ch]` | 8259A 仮想化 | `v86_pic_io`, `v86_pic_get/set_isr`, `v86_pic_is_reboot` |
| `kernel/v86_pit.[ch]` | 8253 仮想化 | `v86_pit_io` |
| `kernel/v86_bios.[ch]` | INT 11h/12h/18h/1Ch/29h | `v86_bios_int*` |
| `kernel/v86_disk.[ch]` | INT 1Bh (FDD のみ) | `v86_bios_int1b`, `v86_disk_set_file/_physical/_clear` |
| `kernel/v86_session.[ch]` | セッション制御・Auto-Typer・脱出ホットキー | `v86_boot_freedos`, `v86_boot_physical_fdd`, `v86_session_on_tick` |
| `kernel/v86_test.c` | COM 実行テスト ＋ longjmp 復帰関数 | `v86_test`, `v86_test_exit`, `v86_current_jmpbuf` |
| `kernel/v86_debug.[ch]` | シリアル／ファイル ダンプ | `v86_debug_dump_session` |
| `kernel/v86_bda.h` | PC-98 BDA オフセット定数 | (定数のみ) |
| `kernel/isr_stub.asm` | `isr_stub_13` (#GP) で V86 を判定し `v86_gp_handler` へ／`irq_stub_0` から `timer_handler`→`v86_inject_timer_irq` | — |
| `kernel/isr_handlers.c` | `timer_handler` 内で `v86_active` を見て注入 | — |
| `drivers/kbd.c` | 物理キー受信時に **BDA キーバッファ** に直接書き込む | — |
| `programs/cmds/vdos.c` | ユーザコマンド (`vdos [path] [cmdline]`) | KAPI 経由 |
| `programs/system/vdosquit.asm` | ゲスト内から **OUT 0xFE** で脱出する小プログラム | — |

実行フロー (1セッション):

```
shell/vdos.c
  → kapi->sys_v86_boot_freedos / sys_v86_boot_physical
    → v86_boot_freedos / v86_boot_physical_fdd  [v86_session.c]
       1. V86Session 構造体を初期化 (Auto-Typer 文字列を保持)
       2. (file モード時) D88/FDI/RAW ヘッダ判定して IPL を読む
       3. v86_mem_setup()       [v86_mem.c]   ページテーブル・IVT・BDA・iomap
       4. v86_pic_init()                       仮想 PIC リセット
       5. v86_pit_init()                       仮想 PIT リセット
       6. v86_disk_set_file/_physical          ディスクバックエンドを設定
          ※ D88は判定ブランチ内で呼ぶ。FDI/RAWは判定後に呼ぶ (d88_detected フラグで分岐)
       7. IPL を 0x1FC0:0x0000 にコピー
       8. v86_session_run_core():              [v86_session.c]
            - struct v86_context 作成 (eip=0, cs=0x1FC0, vm=1, if=1)
            - tss_set_esp0(v86_kstack 末尾)
            - exec_setjmp(v86_session_jmpbuf)
            - v86_enter(&ctx)                  IRETD で V86 突入
              ↓ #GP 発生のたびに v86_gp_handler が呼ばれ命令デコード
              ↓ V86 終了条件成立 → isr_stub_13 .v86_exit
              ↓ v86_test_exit() → exec_longjmp → setjmp に戻る
            - 後始末 (v86_disk_clear, v86_mem_teardown, v86_restore_screen)
       9. (file モード時) vfs_close
```

---

## 1. 設計上の根本問題

### 1.1 V86 から実 PIC への副作用が一部残る (致命的)
`v86_bios_int18` の `AH=0Ch/0Dh/40h/41h` 内、**実 GDC ポート (`0x62`, `0xA2`) に直接 `outp`** している。これは「BIOS エミュレーション側で副作用を発行している」もので、V86 内のコードがここを通る限り I/O ビットマップの仮想化と整合する。しかし `v86_bios.c` 側で `outp(0x60, ...)` (CSRWコマンド送出) を行っている部分があるため、**Ring 0 のテキスト GDC 状態をゲスト側操作で書き換えてしまう**。これは V86 セッション中ずっと OS32 のコンソール出力と競合する。

該当箇所:
- `v86_bios.c` `case 0x13:` (CSRW コマンド送出, lines ~248-256)
- `v86_bios.c` `case 0x0C/0x0D/0x40/0x41:` (GDC START/STOP)

**修正方針**: 実ポート発行をやめ、TVRAM への直接書き込みのみに統一するか、V86 終了時の `v86_restore_screen` で完全に巻き戻す。後者は既にあるが、セッション中の OS32 描画と同居すると壊れる。

### 1.2 BDA キーボードバッファ HEAD/TAIL の意味が衝突 (バグ)
PC-98 BDA キーバッファは `HEAD=取出ポインタ` `TAIL=入力ポインタ` (NP21/W bios09.c 準拠、コメントにも明記) のはず。しかし:

| 場所 | 操作 | 動作 |
|------|------|------|
| `drivers/kbd.c:198-209` | `tail` を進めて書き込み (生産側) | TAIL=write — 正しい |
| `kernel/v86_session.c:140-156` (Auto-Typer) | `head` を進めて書き込み (生産側) | HEAD=write — **逆** |

両方が「produce」しているため `count` が増えるのに HEAD/TAIL が独立に進み、DOS 側の取出位置が破綻する。Auto-Typer 経路と物理キー経路で **同じバッファに対して逆向きの規約** を採用している。

**修正方針**: NP21/W の規約に統一。生産は TAIL、消費 (DOS 側 `int 18h AH=0`) は HEAD。Auto-Typer も TAIL を進めるよう書き換える。`v86_bda.h` のコメントは生産=TAIL に直す必要あり。

### 1.3 IRQ 注入経路が二系統あり、コード重複と整合性問題
V86 への割り込み注入は **2 つの異なるエントリ** を持つ:

| 経路 | エントリ | スタックフレーム | 使う定数 |
|------|----------|-----------------|----------|
| HW IRQ 注入 | `v86_inject_timer_irq()` (`v86.c`) ← `timer_handler` ← `irq_stub_0` | `push ds/es; pushad` フレーム | `HWIRQ_REG_*` (10..14) |
| GP 内 pending 注入 | `v86_gp_handler` 末尾 | `pushad` + #GP V86 自動 push | `V86_REG_*` (0..17) |

両者で似た push16×3 + CS/IP/IF 操作を行っており、`v86_gp_inject_irq()` と `v86_hw_inject_irq()` という 2 つのヘルパに分離されている。**スタック上のレジスタオフセットが違うだけで本質は同じ**。

問題:
- ISR/IMR/IVT のチェック条件が両者で微妙に異なる (例: GP 経路では IRQ1 で `imr & 2` を見ない、HW 経路では見る)。
- `irq_stub_1` (キーボード IRQ) は `v86_inject_*` を **一切呼ばない** ので、キーボードは BDA バッファ書き込み専用になっている。一方 `v86_gp_handler` 末尾には IRQ1 注入コードが書かれている (line 699-717) — これは `v86_set_pending_irq(1)` が呼ばれたときに動くが、**`v86_set_pending_irq(1)` を呼ぶコードがどこにも無い** ため、永遠に発火しない。実質デッドコード。

**修正方針**: 注入処理を 1 つの関数に統一し、引数で「呼び出し元コンテキスト (HW or GP)」とそのスタックフレーム構造体を渡す。IRQ1 注入は **完全に削除** するか、**有効化して BDA 直接書き込みを止める** か、どちらかに振り切る。現状は二重になっている (BDA に書きつつ仮想 IRQ1 もキューに入っていない)。

### 1.4 v86_active のグローバルフラグはネスト不可
`volatile int v86_active` は単一インスタンス。`current_session` (static) も単一。再入や入れ子セッションは未対応。

- shell から `vdos` 起動中にさらに `vdos` を呼ぶ経路は閉じられていない (kapi 経由なので理論上呼べる)。
- セッション構造体配列化 + アクティブ TSS インデックスでマルチセッション対応するか、明示的に再入禁止チェックを追加する。

### 1.5 longjmp による V86 脱出の TSS ESP0 復元が不正
`v86_session_run_core` で `saved_esp0 = 0x9FFF0UL` と **ハードコード**。元の TSS ESP0 を読み取って保存していない。
- shell 帯域以外から V86 を呼んだとき (例: 将来別タスクから呼ぶとき) にスタックがズレる。
- 修正: `tss_get_esp0()` を追加して保存。

### 1.6 シェル帯域と V86 バッキング RAM の重なり — ✅ 解決済

> **修正済 (2026-05-06)**: `v86_backing_phys` を `pgalloc_alloc_n(160)` で動的確保するように変更。
> バッキングRAMはプログラム空間 (0x400000+) に配置され、シェル帯域 (0x300000-0x37FFFF) とは
> 物理的に分離される。シェル退避バッファ (512KB BSS) も削除済み。
> T1/T2/T4 連続実行で #PF 消失・シェル正常復帰を確認。

~~`v86_mem.c` 冒頭コメント:~~
~~> シェル帯域 (0x300000-0x3FFFFF) をバッキングRAMとして転用する。~~

~~しかし現在の `vdos.c` は **シェル経由で呼ばれており**、Phase 1 の前提が崩れていた。現行コードはシェル帯域を **退避せずに 0 クリア** しており、V86終了後に #PF が 100% 再現していた。~~

~~**修正方針**: シェル帯域全体を別物理ページに退避→V86 動作→V86 終了後に書き戻し。または、バッキング RAM をシェル帯域の外 (例: 0x400000+) に移す。~~

### 1.7 GVRAM/TVRAM の DOS 上書きが永続化
V86 終了後 `v86_restore_screen()` はパレット・GDC モード・GRCG を OS32 デフォルトに戻すが、**TVRAM/GVRAM の中身そのものは復元しない**。
- DOS が描いた文字が画面に残る。
- 必要なら `v86_mem_setup` 直前に TVRAM (0xA0000-0xA1FFF) と GVRAM 各プレーンを別領域へ退避しておき、終了時に復元する。

### 1.8 Rust 移行計画が 0 行
`v86.c:8` のコメント:
> Phase 0: 基本命令デコーダ ... 後のフェーズで Rust (os32_v86) に移行予定。

Rust 側の `os32_v86` クレートは存在しない (`programs/rust/` を確認済み)。コメントは誤誘導なので削除するか、本当に Rust 化するならクレート骨格 (FFI + 命令デコード) を起こす。

---

## 2. 命令デコーダ (`v86_gp_handler`) の未対応

`v86.c` の `switch (opcode)` で実装されているのは以下のみ:

| Opcode | 命令 | 状態 |
|--------|------|------|
| 0xCD nn | INT n | 一部実装 (§3 参照) |
| 0xCF | IRET | OK |
| 0xFA | CLI | 仮想 IF クリア |
| 0xFB | STI | 仮想 IF セット |
| 0x9C | PUSHF | 16bit のみ |
| 0x9D | POPF | 16bit のみ |
| 0xF4 | HLT | 終了要求チェックのみ (NOP 扱い) |
| 0xE4/0xE5/0xE6/0xE7/0xEC/0xED/0xEE/0xEF | IN/OUT (8/16bit) | OK (PIC/PIT 仮想化付) |

**未対応 (default 経路で V86 セッションが即終了する)**:

| Opcode | 命令 | 影響 |
|--------|------|------|
| 0x66 | OPERAND_SIZE prefix | **致命**。`pushfd`, `popfd`, 32bit IN/OUT 全て未対応。32bit DOS Extender 系は即死。 |
| 0x67 | ADDRESS_SIZE prefix | 同上 |
| 0x26/0x2E/0x36/0x3E/0x64/0x65 | seg-override prefix (ES/CS/SS/DS/FS/GS) | I/O 命令と組み合わさったときデコード不能 |
| 0xF0 | LOCK prefix | NP21/W FreeDOS は出さないが、汎用化なら必要 |
| 0xF2/0xF3 | REPNE/REP prefix | **REP INSB/OUTSB が頻出**。デコーダ側で文字列 IN/OUT のループ + ECX 操作が必要 |
| 0x6C/0x6D | INSB/INSW | 文字列入力 (BIOS の VRAM 読み出しなどに登場) |
| 0x6E/0x6F | OUTSB/OUTSW | 文字列出力 |
| 0xCC | INT3 | デバッガ用ブレークポイント — DOS の DEBUG.COM が落ちる |
| 0xCE | INTO | OF=1 時のみ INT 4 — 通常通らないが完全性のため |
| 0x9B | WAIT/FWAIT | FPU 同期 (ack のみで OK) |
| 0xF1 | INT1/ICEBP | 通常通らない |
| 0x0F xx | 2 バイトオペコード (LIDT/LGDT/MOV CRn/SMSW/...) | DOS-Extender や Win3.1 が触る |
| 0xE8/0xE9 (JMP near) | 通常はトラップしない | (真のリアルモード命令なので #GP は出ない、参考) |

**特に重要なのは 0x66 prefix と 0xF3/0xF2 prefix**。これらが付いた CLI/STI/IN/OUT/PUSHF/POPF を 1 命令としてデコードするため、

```
[prefix bytes] [primary opcode] [imm/operand]
```

の長さ計算とフェッチを統合する小さなデコードフロントエンドが必要。`v86.c:317` の `switch (opcode)` を「prefixes をループで吸ってから primary opcode に分岐」に書き換えるのがミニマル変更。

---

## 3. INT エミュレーション (host-side) の網羅状況

`v86_gp_handler` から host 側にディスパッチされる INT:

| INT | host 関数 | カバレッジ | 主な未実装 |
|-----|-----------|-----------|-----------|
| 0x11 | `v86_bios_int11` | 機器構成: 常に 0x0000 を返す (FDD 1台) | 実機の構成検出と乖離。HDD 装着時に値を変える経路無し |
| 0x12 | `v86_bios_int12` | BDA 0x0413 の値を返す | 拡張メモリ未対応 (常に 640) |
| 0x18 | `v86_bios_int18` | AH=00,01,02,03,04,05,0A,0B,0C,0D,0E,0F,10,11,12,13,14,15,16,17,18,19,1A,1B,40,41,42,43 | AH=0Eh ファンクションキー、AH=30h CRT モード、AH=31h テキストアトリビュート、AH=39h センス、AH=44h+ 拡張グラフィック、AH=4xh の多くの拡張 |
| 0x1B | `v86_bios_int1b` | FDD: 機能 01,02,03,04,05,06,07,0A | **書き込みプロテクト未反映**。**機能 0Eh (FORMAT) 未実装**。**HDD (DA/UA=0x80) 未対応**。**640KB FDD (0x30) 未対応**。**セクタ長 N!=3 全エラー** |
| 0x1C | `v86_bios_int1c` | 00h (日時読出) | 01h は ack のみ (実際の RTC 設定無し)。02h以降未対応 |
| 0x20 | (v86.c 内特殊処理) | DOS Terminate → V86 終了 | OK |
| 0x21 | AH=4Ch のみ特殊 | 残りは IVT へフォールスルー → DOS 自身が処理 | DOS が常駐していない構成では全 INT 21h が IRET ダミーに行き着く |
| 0x29 | `v86_bios_int29` | 1 文字出力 | 全角 (Shift-JIS) は 1 バイトずつしか処理されない |

`default` の通常 INT パス (`v86.c:409-426`) は **IVT を読んでハンドラに転送**。IVT が初期値 (0x0050:0x0000 ダミー IRET) のままだと「呼んだら即 IRET」され、**ゲストは戻り値が初期値のまま成功したと誤認**する (キャリーフラグもクリアされない)。これは OS32 視点では「未実装でも落ちないので楽」だが、ゲスト視点では潜在バグの温床。

**修正方針**:
- 主要 INT (00-08h, 0Dh, 19h, 2Fh, ...) のダミーで CF=1 + AH=エラーコードを設定する補助ハンドラを `v86_mem_setup` 時に IVT に登録しておく。
- もしくは `v86_gp_handler` で「IVT がダミーのまま」を検知したらシリアルログに通報し、ホスト側のスタブを呼ぶ。

---

## 4. PIC (8259A) 仮想化の不足

`v86_pic.c` の状態モデル:

```
struct { u8 imr, isr, irr, read_isr, icw_state, icw4_needed; } vpic[2];
```

問題点:

1. **IRR が一切更新されない**。`v86_set_pending_irq` も `v86_inject_timer_irq` も IRR には触らない。`pic_read_cmd` で IRR を返す経路はあるのに、書き込みは存在しない。OCW3=0x0A で IRR 読み出しを期待するゲストには嘘の値 (常に 0) が返る。
2. **AEOI モード (ICW4 bit1) を見ていない**。ICW4 自体を `pic_write_data` の case 3 で読み捨て。AEOI の DOS 環境では永遠に EOI が来ないと誤判定する経路はないが、将来 IRR を実装するなら一緒に対応必要。
3. **特殊マスクモード (OCW3 SMM/ESMM)** 未対応。
4. **カスケード EOI 伝搬無し**。スレーブ PIC (idx=1) からの IRQ で ISR がセットされるとき、マスタの IR7 (=カスケード入力) も自動でセットされるべきだが、そのロジック無し。スレーブ EOI のあとマスタにも EOI が必要だが、ゲストが両方発行するとして「マスタ側の対応 ISR ビット」を立てていないので ISR=0 → no-op で通過する。
5. **OCW1 (IMR 書き込み) のタイミング判定**。`pic_write_data` は `icw_state` が 0 のときに IMR とみなすが、ICW シーケンス未完了で一旦 0 に戻った後 (ICW2/3/4 失敗系) の挙動が undefined。
6. **`v86_pic_io` のマッチが master/slave 各 2 ポートだけ**。PC-98 では `0x100` 系のセカンダリ PIC ポートはほぼ無いが、`0x09` (slave 専用 mock?) は存在しない。とはいえ追加すべきはまず IRR 更新ロジックの方。

---

## 5. PIT (8253) 仮想化の不足

`v86_pit.c`:

1. **`estimate_counter()` が完全にフェイク**: `rv/2` か `rv/4` を返すだけ。タイミングループ (BIOS の `_DELAY` ルーチン、ベンチマーク、CPU 速度検出) はナンセンスな値を読む。
2. **Counter#0 の reload を変えてもタイマ IRQ レートは変わらない**。OS32 自身の 100Hz IRQ がそのまま注入される。ゲストが reload=0x800 (4倍速) を期待しても 100Hz のまま。`v86_inject_timer_irq` 内で「reload_value に応じて N tick に 1 回しか注入しない」ロジックが必要。
3. **モード差を見ていない**: mode 0/1/2/3/4/5 全部 mode 3 同等で扱っている。実 DOS では OneShot (mode 0) も使う。
4. **Read-Back コマンド (SC=11)** 無視。
5. **Counter Latch コマンド** はあるがラッチ後の二重ラッチ動作は未検証。
6. **重複ポート (3FD9h/3FDBh/3FDDh/3FDFh)** はテーブルに含めているが、コマンドポート 0x3FDF と 0x77 を独立に扱うかは曖昧。

---

## 6. ディスク (`v86_disk.c`) の未実装と決め打ち

| 項目 | 現状 | 問題 |
|------|------|------|
| サポートデバイス | 1MB FDD UNIT#0 (DA/UA=0x90) のみ | 640KB FDD (0x30)、HDD (0x80)、SCSI (0xA0) 未対応 |
| ジオメトリ | 77×2×8×1024 固定 | 1.44MB (80×2×18×512) や 1.2MB (80×2×15×512) 不可 |
| セクタ長コード CH | 3 (1024B) のみ受理、それ以外 0xC0 エラー | DOS が CH=2 (512B) で叩くフォーマット FDI に未対応 |
| 機能 0Eh FORMAT | 未実装 | DOS フォーマット不可 |
| 機能 09h WRITE ID | 未実装 | フォーマット系不可 |
| READ ID (0Ah) | 戻り値ハードコード (R=1, N=3) | 実フォーマットと乖離 |
| ライトプロテクト | 常に「無し」 | ホストファイルが R/O でも書き込みは成功扱い |
| 複数セクタ転送 | 実装済 (READ/WRITE) | OK |
| BDA FDC 結果バッファ | 0x0564 + us*8 を更新 (READ/WRITE のみ) | INITIALIZE/SENSE/RECAL では更新しない |
| エラー注入 | 常に成功 | リトライ動作の検証ができない |

`v86_disk_set_file` のサイズチェックが `fdd_image_size` を渡された値そのままにしている。FDI ヘッダ後の純データ長と一致するかは呼び出し側 (`v86_session.c`) の責任。

---

## 7. メモリ仮想化 (`v86_mem.c`) の留意点

### 7.1 「方法 C」リマップは不可逆
0x8F000-0x9FFFF (カーネルスタック領域) を最初に物理 0x38F000-0x39FFFF にコピーし、以降そこを使い続ける。`v86_mem_teardown` でも逆コピーしないので、**1 回でも `v86_mem_setup` を呼ぶと元の物理 RAM 0x8F000-0x9FFFF は永久に放棄される**。
- shell band (0x300000-0x3FFFFF) のうち 0x38F000-0x39FFFF はカーネルスタック専用に占有された状態が残る。
- shell 側のメモリ管理が同領域を割り当てないことが暗黙の前提。memmap 上で明示的に予約する変更が必要。

### 7.2 1MB ラップアラウンド (HMA) 不対応
`v86_phys_addr` は `linear &= 0xFFFFF` で 1MB 強制ラップ。実 DOS は A20 線で 1MB 直後を直接アクセスする (HMA, 0xFFFF:0x0010+)。OS32 は A20 概念無し。HMA 利用 DOS (HIMEM.SYS) は不可。

### 7.3 0xC0000-0xDFFFF / 0xE8000-0xEFFFF を identity & R/O にしている
これは **拡張 ROM BIOS とバンクメモリ領域** に該当する。OS32 がこの範囲のページに何か書くと V86 終了後に R/O のままになる可能性 (teardown で R/W に戻すのは 0xC0000-0xDFFFF と 0xE8000-0xEFFFF の 2 領域のみで、`PTE_USER` クリアと書き込みフラグ復元が混在しており、同領域を使う他システムが存在する場合に予期せぬ R/O 状態になり得る)。

### 7.4 I/O ビットマップ許可ポートが過剰広い可能性
`tss_iomap_allow` で許可しているもの (V86 から実機に直接抜けるポート):

```
0x41, 0x43            キーボード 8251
0x60, 0x62, 0x64, 0x66, 0x68, 0x6A   テキスト GDC
0x70, 0x72, 0x74, 0x76, 0x78, 0x7A   CRTC
0x7C, 0x7E            GRCG
0xA0-0xA6, 0xA8-0xAE  グラフィック GDC + パレット
0x04A0-0x04AE         EGC
0x0188-0x018E         FM 音源
0x20                  カレンダ
```

この穴を通じてゲストは **OS32 のテキスト GDC、グラフィック GDC、EGC、FM 音源を直接いじれる**。OS32 終了後 `v86_restore_screen` でパレット・GDC モードは戻すが、**FM 音源 (OPN レジスタ)、EGC レジスタ、GRCG タイル設定** はリストアしないので、サウンドや EGC 状態が壊れたまま戻る。

修正方針: 終了時に各デバイスの保存/復元シーケンスを追加するか、許可ポートを縮小して BIOS レイヤで仮想化する。

---

## 8. セッション (`v86_session.c`) の問題

1. **Auto-Typer と物理キーの BDA 書き込み衝突** (§1.2)。
2. **強制脱出ホットキー** (`Ctrl+GRPH+DEL`) は `kbd_peekkey()` の返す **直近 1 件** しか見ない。複数キーがバッファに溜まっていると拾わない可能性。コミット履歴によると以前 F12 だったのを Ctrl+GRPH+DEL に変更済み。
3. **Auto-Typer の文字コード**: ASCII のみ。改行は `\r`/`\n` を `0x0D` に変換 + scancode `0x1C` (Enter) で 1 文字注入。スキャンコードは ASCII から逆算していない (常に 0)。FreeDOS 側で「キーバッファの上位バイト」を見るプログラムは破綻する。
4. **タイムアウト無効**: `V86_TIMEOUT_TICKS = 0` のため `v86.c` 内の **タイムアウト系コードは到達不能** (line 282-296, 821-843)。HLT 強制注入による安全脱出機構が常時無効。実装を残すなら有効値 (例: 6000 = 60秒) を設定。または機能ごと削除。
5. **セッション終了時に FDD イメージファイルがクローズされない経路**: `v86_boot_freedos` は OK。`v86_boot_physical_fdd` は FD を持たないので無関係。
6. **戻り値 0/-1 のみ**: 終了理由を呼び出し元に返す API が無い (`current_session.exit_reason` は static で外部参照不能)。

---

## 9. デバッグ機能 (`v86_debug.c`) の制限

- `v86_debug_dump_session` は `v86_debug_enabled` 変数で制御。**シェルから enable する API がない** (vdos.c でも触らない)。
- ファイル書き出しは `/host/v86_fdos_log.txt` `/host/v86_gptrace.txt` 固定。`/host/` は NP21/W のホストファイルシステム経由マウントを想定 (詳細は `tools/os32_server.py`)。実機ブート時は失敗するがエラーは握り潰される。
- リングバッファサイズ: GP トレース 128、ディスクログ 64、I/O 統計 16 ポート。長時間セッションでは溢れる。

---

## 10. ASM スタブとカーネル統合

### 10.1 isr_stub_13 (#GP) のスタックフレーム前提
コメントに従うと V86 #GP のフレームは:
```
[error_code(4)] [EIP(4)] [CS(4)] [EFLAGS(4)]
[ESP(4)] [SS(4)] [ES(4)] [DS(4)] [FS(4)] [GS(4)]
```
`pushad` 後の `regs[]` は `V86_REG_*` (v86.h:32-49) の配列。  
`v86.h` 側の定義は **17 要素まで (GS=17)** だが、**実際の `regs` 配列は最大インデックスがコードに散見**。`v86_gp_handler` は GS まで届く参照を実際にはしていないが、近い将来「セグメントオーバーライド付き I/O」を実装する際に GS=17 を読む必要がある。アクセスは安全。

### 10.2 irq_stub_0 と timer_handler 経由の注入
`timer_handler(u32 *regs)` の `regs` は `irq_stub_0` の `pushad` の先頭。これに `HWIRQ_REG_*` 定数 (v86.c:751-755) を当てて V86 の自動 push されたフレームへアクセスしている。**`irq_stub_0` 側の push 順序が変わると `HWIRQ_REG_*` が壊れる**。コメントは付いているが、保守時に同期忘れリスクが高い。
- 対策: `HWIRQ_REG_*` を `kernel/isr_stub.asm` のラベルから派生するアサーションで縛るか、共通ヘッダ `irq_frame.h` を作って ASM 側でも include する。

### 10.3 IRQ1 の V86 経路欠落
`irq_stub_1` は `kbd_irq_handler()` を呼ぶだけで、`v86_inject_*` は呼ばない。`drivers/kbd.c` のコメント (line 109-115) で「INT 09h 注入は行わない」と明言。つまり:
- ゲストの INT 09h ベクタ (`IVT[9]`) を呼ぶ経路は **存在しない**。
- ゲスト DOS が「キーボード割り込みハンドラを差し替える」プログラム (キーマップ等) を実行しても、その新ハンドラは絶対に呼ばれない。
- 一方 `v86.c:699-717` で IRQ1 注入のコードは生きている (`v86_pending_irq & 2` で発火)。`v86_set_pending_irq(1)` を呼ぶコードが無いのでデッドコード。

**選択肢**:
- A: BDA 直書き運用に振り切り、`v86.c:699-717` の IRQ1 注入コードを削除。
- B: BDA 直書きをやめて、`kbd_irq_handler` で `v86_set_pending_irq(1)` を呼ぶ。Auto-Typer も BDA に書くのではなく仮想 IRQ1 注入に変える。

### 10.4 IRQ2-15 全滅
RTC (IRQ8?), FDC IRQ11, RS232C IRQ4, マウス IRQ13 は **どれも V86 にリフレクトされない**。`fdc_irq_handler` `serial_irq_handler` `mouse_irq_handler` は OS32 内のフラグ更新のみ。
- DOS のシリアル受信割り込みは届かない。
- マウスドライバ (MOUSE.SYS) は割り込みベースなので動かない。
- FDC は v86_disk が同期 I/O で代替している (FDD は問題なし)。

---

## 11. 終了経路 (`v86_request_exit` / `v86_exit_request`) の流れ

```
[ゲスト動作]                          [OS32 側]
OUT 0xFE  (vdosquit.com)
INT 20h
INT 21h AH=4Ch
OUT F0h   (リブート)
CS >= 0xF000
未対応 opcode
タイムアウト (現在無効)
Ctrl+GRPH+DEL
   ↓
v86_request_exit(reason)
   - current_session.exit_reason に格納
   - v86_exit_request = 1
   ↓
v86_gp_handler が return 1
   ↓
isr_stub.asm .v86_exit
   - SS/DS/ES/FS/GS = 0x10 復元
   - call v86_test_exit
   ↓
v86_test_exit (v86_test.c:192)
   - sti
   - exec_longjmp(v86_current_jmpbuf)
   ↓
exec_setjmp が return 1 で復帰
   ↓
v86_session_run_core 後始末
```

問題点:

1. `v86_test_exit` が共通脱出関数になっているのは **歴史的事情**。`v86_test.c` の名前が示唆する通り元々はテスト用だが、`v86_session` も同じ jmpbuf 経路を使っている (`v86_current_jmpbuf` を差し替えるだけ)。命名と責務がズレているので `v86_runtime.c` か `v86.c` 本体に移すと整理される。
2. `v86_current_jmpbuf` が NULL の場合 `for (;;) hlt` でハング。テスト時のフォールバックだが本番では危険 (kpanic 等に置き換えるべき)。
3. **HLT 命令での終了**: `case 0xF4:` は `v86_exit_request` がセットされていれば 1 を返すが、未セットなら NOP 扱い。ゲストが何の終了要求もなしに HLT した場合、無限に GP ループする (HLT のたび GP 発生)。ループにはなるが無害ではない (CPU を 100% 食う)。

---

## 12. 既知の落とし穴 (デバッガ向けの罠)

1. **DS/ES クリア問題**: V86→Ring0 遷移時に CPU が DS/ES/FS/GS を **0x0000** にクリアする。各 ISR 冒頭で `mov ax, 0x10; mov ds/es, ax` するのを忘れると、Cハンドラ内の通常ポインタアクセスが #PF を呼ぶ。`isr_stub.asm` 中の `★` コメントは全部この対策。新しい IRQ ハンドラを追加するときは要注意。
2. **EFLAGS.VM は IRETD でしか落とせない**。Ring0 内で `popfl` しても VM は無視される。だから longjmp 経路で完全脱出している。
3. **TSS の I/O ビットマップは Ring3 の I/O のみ参照される**。V86 (CPL=3 互換) では IOPL を見ずに iomap を見る。`v86_mem_setup` の `tss_iomap_allow` は V86 中のみ有効で、終了時 `tss_iomap_deny_all` で全閉に戻している。Ring3 の通常ユーザプログラムは IOPL=0 なので全 I/O が #GP になる前提。
4. **バッキング RAM 物理アドレスは pgalloc 動的確保**: V86 起動のたび `pgalloc_alloc_n(160)` で確保され、終了時に `pgalloc_free_n()` で解放される。シェル帯域 (0x300000-0x37FFFF) とは物理的に分離されており、衝突は発生しない。
5. **IPL アドレス 0x1FC0:0x0000 = 0x1FC00**: バッキング RAM 内なので物理は `v86_backing_phys + 0x1FC00`。`vfs_read_fd(fd, ipl_dst, IPL_SIZE)` で書き込むときに `ipl_dst` は `v86_phys_addr` 経由で物理アドレスに変換済。
6. **FDI/RAW での `v86_disk_set_file` 呼び忘れ**: `v86_boot_freedos()` でメディア形式判定後に `v86_disk_set_file()` を呼ぶ際、D88ブランチは内部で呼び済みのため外側では呼ばない。FDI/RAW は判定後に外側で呼ぶ必要がある。条件分岐には `d88_detected` フラグを使うこと (`media_detected` を使うと FDI で呼ばれない)。2026-05-06 修正済。

---

## 13. デバッガ向け FAQ

**Q. `case 0xCD` 内の INT N で `intno` が想定外、なぜ?**  
A. `ip = v86_phys_addr(cs, eip)` が指すバイト列を `*ip` で読む。前にプレフィックス (0x66 等) があるとデコードがズレる。§2 の prefix 対応が無いことを思い出す。

**Q. キー入力が反映されない**  
A. 物理キーは `kbd_irq_handler` で BDA[0x0524..0x0528] に書き込まれる。Auto-Typer は BDA[0x0524] (HEAD) を進めるが、消費側 (FreeDOS の INT 18h AH=00h は host 側エミュレーション) は OS32 リングバッファ (`kbd_trygetkey`) を見るため**両方の経路が同期していない**。直近の修正対象。

**Q. タイマー割り込みが届かない**  
A. `v86_irq0_call_count` は 100Hz でカウントされるはず。`v86_irq0_inject_count` が 0 なら IF=0 か ISR ビットが立ったまま。`v86_debug_dump_session` のシリアル出力で各カウンタを確認。

**Q. ディスク READ が 0xC0 を返す**  
A. CH (`sector_len`) が 3 以外。ゲストが BDA 0x0564+6 (N 値) を CH に渡す。前のセッションで BDA がクリアされて 0 になっている可能性。FORMAT 後に N=3 を書き戻す処理が無いのも一因。

**Q. ディスク READ が 0xE0 を返す (全セクタ)**  
A. `fdd_fd < 0` — `v86_disk_set_file()` が呼ばれていない。FDI 形式ではメディア判定で `media_detected = 1` になるが、`v86_disk_set_file` の呼び出し条件を `!media_detected` にすると FDI で呼ばれない。`d88_detected` フラグで分岐すること (2026-05-06 修正済)。

**Q. V86 終了後にシェルが暴走する**  
A. ✅ **解決済**: `v86_backing_phys` を `pgalloc_alloc_n(160)` で動的確保するように変更し、シェル帯域との衝突を根本解決 (2026-05-06)。

**Q. `unhandled INT 18h AH=??` が大量に出る**  
A. `v86_bios_int18` の switch default。シリアルログに 10 件まで出る (それ以降は静音)。AH 値を見て §3 表の未実装欄に追加する。

---

## 14. 推奨修正の優先度

| 優先度 | 項目 | 参照節 |
|--------|------|--------|
| ★★★ | BDA キーバッファ HEAD/TAIL 規約統一 | §1.2 |
| ★★★ | ~~シェル帯域とバッキング RAM の重なり解消~~ ✅ 解決済 | §1.6 |
| ★★★ | 0x66 prefix 対応 (デコーダ最優先) | §2 |
| ★★★ | REP INSB/OUTSB 対応 | §2 |
| ★★ | IRQ 注入の二系統統合 + IRQ1 経路の決定 | §1.3, §10.3 |
| ★★ | PIT カウンタ推定の真面目化 (タイマレート反映) | §5 |
| ★★ | I/O ビットマップ許可ポートの状態退避 (FM/EGC/GRCG) | §7.4 |
| ★★ | ライトプロテクト反映 (ディスク) | §6 |
| ★★ | 未実装 INT のダミー応答で CF=1 を立てる | §3 |
| ★ | TVRAM/GVRAM の退避・復元 | §1.7 |
| ★ | TSS ESP0 の保存値ハードコード除去 | §1.5 |
| ★ | HDD (DA/UA=0x80) サポート | §6 |
| ★ | Rust 移行コメントの整理 (削除 or 骨格作成) | §1.8 |
| ★ | デッドコード (タイムアウト経路、IRQ1 注入経路) の決断 | §1.3, §8.4 |

---

## 15. 参考: 出典

- `docs/PC9800Bible/INDEX.md` — PC-9800 ハードウェア
- NP21/W ソース (npc/np21w):
  - `bios/bios09.c` — キーボード BIOS
  - `bios/bios1b.c` — ディスク BIOS
  - `cpucore/cpu_io.c` — I/O ハンドラ
- `OS32_SPEC.md` 第2部 — メモリレイアウト
- `KAPI_SPEC.md` — `sys_v86_*` API シグネチャ

---

末尾: 本書は 2026-05-06 時点のソースを基に作成。
最新の状態は `kernel/v86*.c` を直接参照のこと。
