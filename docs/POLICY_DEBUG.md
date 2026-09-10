# OS32 デバッグポリシー

OS32 のバグ調査・修正・検証における公式手順とプロセスを定める。
ベアメタル OS 特有の落とし穴や、AI との協調デバッグにおけるルールを含む。

開発全般のポリシーは [POLICY_DEV.md](POLICY_DEV.md) を参照。
既知のハードウェア制約と技術ノートは [10_notes.md](10_notes.md) を参照。

---

## §1. デバッグの基本原則

### 壊す前に理解する

コードを変更する前に、必ず以下を行う:

1. **現象を正確に記述する**: 何が起きているのか、何が期待される動作なのか
2. **仮説を立てる**: 最低2つ以上の原因候補を優先度順に列挙
3. **検証計画を作る**: どの仮説をどの順序で検証するか

> 「なんとなくここが怪しい」でコードを修正してはならない。仮説が明確でない修正は、別のバグを生む温床になる。

### 仮説駆動デバッグの3ステップ

```
1. 仮説を立てる (Hypothesize)
   ↓
2. 最小限の変更で検証する (Verify)
   ↓
3. 結果を記録し、次の仮説に進む (Record & Iterate)
```

---

## §2. バイナリ反映の確認 (最優先)

**デバッグの最初のステップは「バイナリが正しくデプロイされているか」の確認である。**

NP21/W 上でコード変更が反映されていないように見える場合、ロジックの調査に進む前に以下を**必ず**確認する:

### チェックリスト

| # | 確認項目 | 方法 |
|---|---------|------|
| 1 | ビルドタイムスタンプが更新されているか | `ver` コマンドで Build タイムスタンプを確認 |
| 2 | カーネルのロードパスとデプロイパスが一致しているか | `config.h` の `SYS_SHELL_BIN` 等 と `deploy.yaml` の `guest:` パスを照合 |
| 3 | NHD 上のファイルが最新か | ファイルのサイズ・MD5 がビルド成果物と一致しているか確認 |
| 4 | `make clean` が必要ではないか | KernelAPI 構造体変更後は古い `.o` ファイルが ABI 不整合を起こす |

> ⚠️ **教訓**: デバッグ出力が反映されない = 「コードのバグ」ではなく「バイナリが古い」可能性を**最初に**排除すること。これはプロジェクト開始以来、何度も繰り返された最も時間を浪費する問題である。

### カーネルの到達確認は kselftest で行う

`kernel/kselftest.c` のブート時セルフテストは毎回走り、失敗項目を赤で表示する。
画面を見ない場合は `kselftest_pass` / `kselftest_fail` を `emu_read_mem` (または
`/api/mem`) で読む。**番地は新しい `build/out/kernel.map` から引くこと** —
古い map の番地で読んだ値や、初期化前のゼロ値を合格と判定しない。

- `kstring` / `kmalloc` / `kprintf` のプリミティブを触ったら、kselftest に項目を足す。
- `userland/tests/klibc_test` は newlib をリンクするので、カーネル側の検証にはならない。

---

## §3. 仮説の提示と検証プロセス

### プロセスフロー

```
  ┌──────────────────────────────┐
  │ 1. 現象の整理                  │
  │    - 何が起きているか          │
  │    - 何が期待される動作か      │
  └──────────────┬───────────────┘
                 ▼
  ┌──────────────────────────────┐
  │ 2. 仮説リスト作成              │
  │    - 優先度順に3つ以上列挙     │
  │    - 各仮説に検証方法を付記    │
  └──────────────┬───────────────┘
                 ▼
  ┌──────────────────────────────┐
  │ 3. 仮説を1つ選び検証          │
  │    - 最小限のコード変更で       │
  │    - ビルド → デプロイ → テスト │
  └──────────────┬───────────────┘
                 ▼
           ┌──────────┐
           │ 解決した？ │
           └─────┬────┘
          Yes ┌──┴──┐ No
              ▼     ▼
           完了   次の仮説へ
                  (結果を記録)
```

### AI との協調時のルール

1. **コードを変更する前に**、仮説リストをユーザーに提示して承認を得ること
2. 仮説を1つ検証して棄却されたら、次の仮説に移る前に**結果をユーザーに報告**し、方針を確認する
3. **3回リトライルール**: デバッグコードの追加 → ビルド → デプロイ → テスト のサイクルを3回以上繰り返しても解決しない場合は、必ず一度立ち止まってユーザーに状況を報告する

---

## §4. よくある落とし穴と教訓集

過去に OS32 開発で実際に遭遇し、大きな時間を費やした問題のカタログ。
新しい問題に直面した際は、まずこのリストに該当するものがないか確認すること。

### 4-1. ABI 不整合 (KernelAPI 構造体変更後の clean 忘れ)

- **現象**: `malloc` が常に ENOMEM で失敗する、KAPI 関数が間違った引数を受け取る
- **原因**: `os32_kapi_shared.h` を変更したが `make clean` をせずに差分ビルドした結果、古い `syscalls.o` が構造体の旧レイアウトでリンクされた
- **対策**: KernelAPI 関連ヘッダを変更したら**必ず `make clean` → `make all`**

### 4-2. ブートローダーの `org` 設定

- **現象**: PM 遷移直後にハングする、データ参照が壊れる
- **原因**: 第2ステージローダーが `org 0` でアセンブルされた (正しくは `org 8000h`)
- **対策**: DS=0 で絶対アドレス参照を行うローダーは必ず `org 8000h` で記述する

### 4-3. GCC 最適化と配列境界外アクセス (UB)

- **現象**: FAT12 のファイル名読み取りで後半 (ext[3]) が空になる
- **原因**: `name[8]` と `ext[3]` を連続配列として `name[11]` でアクセスした結果、GCC `-O2` が配列境界外アクセスを UB として最適化除去
- **対策**: 各フィールドは必ずその名称で個別にアクセスする

### 4-4. インラインアセンブラでのレジスタ保存

- **現象**: `exec_run()` 後にカーネルスタックが壊れる
- **原因**: 旧 ESP を `eax` に保持したまま関数呼び出しを行い、呼び出し先で `eax` が破壊された
- **対策**: レジスタではなく**`static` 変数 (メモリ)** に保存・復帰する

### 4-5. BSS ゼロクリアの必要性

- **現象**: 起動直後に `exec_nest_level` 等のグローバル変数がゴミ値で動作不良
- **原因**: ベアメタル環境では CRT0 相当の BSS ゼロクリアを自前で行う必要がある
- **対策**: `kentry.asm` 内で `rep stosd` により BSS セクション全体をゼロクリア

### 4-6. ext2 パーティション重複

- **現象**: ファイルシステムのマウントが失敗する、スーパーブロックが壊れる
- **原因**: カーネルバイナリの成長により、NHD イメージ上でカーネル後部が ext2 スーパーブロックに重なった
- **対策**: ext2 の開始をシリンダ12 (LBA 1632) に配置 (詳細は [NHD_FORMAT.md](NHD_FORMAT.md))。カーネル直接配置経路の成長猶予は 128KB (LBA 262 の sqlite.bin 領域まで)

### 4-7. シリアル API タイムアウトによる「ハング」誤診

- **現象**: `exec` 実行後にシェルが復帰しない。`exec_exit` にデバッグ出力を追加しても表示されない
- **原因**: NP21/W デバッグ HTTP API (`curl -X POST http://127.0.0.1:8025/api/cmd`) の `curl -m` タイムアウトが短すぎた。エミュレータの処理速度（~16MHz）でプログラム実行に時間がかかり、応答が返る前にタイムアウト。タイムアウト後はシリアルプロトコルが非同期状態になり、後続コマンドもすべて失敗する
- **対策**:
  - `curl -m` は十分長く設定する（最低15秒、長時間プログラムは60秒以上）
  - タイムアウト発生後は NP21/W を再起動してシリアルを再同期する
  - 「シェルが復帰しない」場合は、まず画面を直接確認する (`/api/tvram` か `/api/screenshot`)

### 4-8. NP21/W 実行中の NHD デプロイが反映されない (ファイルロック)

- **現象**: `make deploy-kernel` / `deploy-nhd` が成功したように見えるのに、修正が実機に反映されない。何度デプロイ+再起動しても古いバイナリが動き続ける
- **原因**: NP21/W が os32.nhd を開いたまま (実行中) の状態では、`nhd_deploy.py deploy` の NHD コピーが失敗またはサイレントに無効化される。パイプで出力を `tail` すると失敗メッセージも exit code も見えなくなる
- **対策**:
  1. NHD デプロイ前に必ず NP21/W を停止する: `taskkill.exe /F /IM np21x64w.exe`
  2. `make deploy-kernel` → `tools/np21w_restart.py` の順で実行
  3. デプロイ後は §2 のチェックリスト通り `ver` の Build タイムスタンプで反映を確認する
- **教訓** (2026-08-05): この問題により「修正が効かない」調査サイクルを3回空転した。§2「バイナリ反映の確認」を最初に行っていれば1回で気づけた

### 4-9. エミュレータ共有メモリとコンパイラ最適化 (volatile欠落)

- **現象**: NP21/W との通信 (例: HostDrvのhypercall) で、エミュレータがステータスをメモリに書き込んでいるにも関わらず、OS32側では値が更新されていない (番兵値のまま) ように見える。
- **原因**: 通信用メモリ構造体に `volatile` 修飾子が付与されておらず、かつ I/Oアクセス関数 (`outp` 等) のインラインアセンブラに `"memory"` clobber（コンパイラバリア）が無かった。このため、GCCの最適化 (キャッシュ) によりメモリの再読み込みが省略され、レジスタに残った古い値が評価されてしまった。
- **対策**: エミュレータが外部から直接読み書きする通信バッファや状態変数は、必ず `volatile` 宣言を行うこと。また、初期化時にエミュレータへの明示的なリセットシーケンスを送信して状態を同期させること。

### 4-10. ビットマップフォントの焼き方 (`tools/gen_font16.py`)

- **現象** (〜2026-08-18): 'W' が 'A'、'M' が 'V' に見える。行がガタつく
- **原因**: IPAex 系は欧文がプロポーショナルで、'W' (14px) や 'M' (13px) を 8px の ANK セルへ中央寄せすると左右が切られる。字ごとの ink box を縦位置の基準にすると 'g' 'j' 'y' や '.' ',' が上下にばらつく
- **対策**: セルからはみ出す字だけ横に畳む。**縦は必ずベースライン基準** (`anchor='ls'`)、縦は縮めない。16x16 の漢字は明朝だと細い横画が飛ぶので本文はゴシック (`assets/fonts/ipaexg16.kcgfont`)。半角 (size15/baseline12) と全角 (size16/baseline13) のベースライン 1px ずれは、16px セルに「漢字の高さ 13 + 欧文 descender 4」が収まらないための妥協 (共通ベースラインにすると漢字が size13 に落ちて見劣りする。比較検証済み)
- **検証**: `--preview out.png` で等倍と 3 倍を並べる。**等倍で読めるかだけが判断基準**。カーネルは起動時に `/sys/font/default.kcgfont` を読む (`kernel.c` の `kcg_load_font`)

### 4-11. 外部プログラムで漢字が全部 □ になる (Unicode→JIS 表の未有効化)

- **現象**: 仮名は出るのに漢字だけ □
- **原因**: カーネルは `/sys/unicode.bin` を `MEM_UNICODE_TABLE_BASE` (0x4A000) へ読み `utf8_set_jis_table_ready(1)` を呼ぶが、このフラグは `lib/utf8.c` の static 変数で、外部プログラムは自分の `utf8.o` を持つため別実体。未有効化だと `unicode_to_jis()` が 0 を返し `kcg_draw_utf8()` が JIS 0x2222 にフォールバックする。仮名はハードコードの範囲変換なので出てしまい、原因が分かりにくい
- **対策**: 表は共有物理メモリにあるので、プログラム側で既知の対応 (U+4E9C→0x3021 など) を数点検証してから `utf8_set_jis_table_ready(1)` を立てる。無条件に立てるとロード失敗時の残骸を表として読む (実例: os32-game `app/main.c` の `enable_kanji_table()`)

### 4-12. deploy.yaml に無いバイナリが NHD 上で stale 化し rshell が沈黙する

- **現象** (2026-08-13, sndctl): KAPI レイアウト変更後、古いバイナリの KAPI 呼び出しが別関数へ飛び、exit 後の `jmp $` で永久スピン。rshell ごと沈黙
- **対策**: 起動対象は PKG 配布でも必ず自層の deploy.yaml に載せる ([V2])。2026-09-04 から `make deploy` / `deploy-nhd` / `deploy-kernel` は同期後に `tools/prune_stale.py` でマニフェストに無いシステム側 *.bin を消す (`NO_PRUNE=1` で一覧のみ、`make prune-stale` で手動確認)

### 4-13. SQLite MEMSYS5 プールの枯渇 (`db_query` が -2)

- **現象** (2026-08-18): game で econ 常時接続 + battle/items/rpg/events の順次ロードの最後の `db_query` が `out of memory`
- **原因**: MEMSYS5 は固定プール (`lib/sqlite3/os32_sqlite_vfs.c`) で、カーネル側 FEP 辞書を含む全接続が共有する。当時 200KB
- **対策**: 384KB に拡大 (SQLite 拡張域 0x200000〜0x2FFFFF 内、残り約 280KB)。エンジンライブラリは `*_init()` の末尾で接続を閉じる。**枯渇の診断は `db_last_error()` を必ず出す** — 戻り値だけでは「テーブルがない」と区別できない。実機で任意 DB を調べるには `dbq` (`userland/tests/dbq.c`)

### 4-14. `mui_pump_input()` がキー待ち行列を食う

- **現象** (〜2026-08-17): os32-game のキーボードショートカットが全部効かない。自動プレイのデバッグタイマはこの回避策だった
- **原因**: `userland/lib/ui/libos32ui_core.c` の `mui_pump_input()` が内部で `kbd_trygetchar()` を呼ぶので、同じフレームでアプリが `kbd_trygetchar()` を呼んでも何も来ない
- **対策**: アプリが 1 回だけ読んで `mui_pump_input_ch(ctx, ch)` に渡す

### 4-15. `exec_exit()` の全回収と `exec_heap_init_at()` による親ヒープ破壊

- **現象**: (a) 2026-08-07 「FEP 候補ゼロ」— `ime on` を実行した ime.bin の終了で常駐辞書の SQLite FD が閉じられた。(b) `ext_cmd1 | ext_cmd2` がキーボード入力で固まる — 1 段目の終了でシェルのパイプバッファが kfree された。(c) `[exec_heap] bad magic feeefeee (double free?)`
- **原因**: 2026-09-03 まで `exec_exit()` は FD ≥ 3 とパイプを所有者に関係なく全部回収していた。(c) は子から戻るとき `exec_heap_init_at()` で親ヒープを再初期化し、先頭ブロックヘッダを書き直したため
- **対策**: 所有者タグ (`res_owner_get()` = ネスト段) で終了段の分だけ回収、カーネル常駐 FD は `vfs_fd_set_protect(fd, 1)`。親へ戻るときは `exec_heap_restore_state()`。仕様は [10_notes §10-9](10_notes.md)

### 4-16. シェルの 2 つのヒープが重なる

- **現象**: `ls > file` の化け、`pipe: out of memory`、double-free 警告
- **原因**: newlib の sbrk (malloc / stdio) と KAPI `mem_alloc` の exec_heap が両方 BSS 終端から始まり互いを上書き
- **対策** (2026-09-03): exec_heap を `MEM_SHELL_HEAP_BASE` (0x380000, 512KB) へ分離。`kernel/paging.c` は 0x380000〜0x3FFFFF を present に保つ (以前は NP ギャップ)。番地は [02_memory.md](02_memory.md)

### 4-17. NHD 作業イメージが `/tmp` にあり、消えても deploy が exit 0 で返っていた

- **現象** (〜2026-09-04): WSL 再起動で `/tmp/os32.nhd` が消え、その状態の `make deploy-nhd` が NHD を書かずに成功扱い
- **対策**: 作業イメージを `build/nhd/os32.nhd` へ移動。無ければ Windows 側から自動 pull (NP21/W 停止中のみ)、失敗は exit 1。`os32-cycle deploy` はブート後にゲストの `/boot/vmkernel.lz4` サイズが手元の成果物と一致しなければ FAIL。**「配備完了」の文言を信じない** — カーネル変更の検証は `kselftest_pass` を新しい kernel.map の番地で読むか、この一致チェックで行う (§2)

### 4-18. テキスト GDC のカーソルが「左下でちかちか」

- **現象** (〜2026-09-04): V86 で DOS を動かした後、DOS 最後のカーソル位置で点滅が残る。OS32 自身はハードウェアカーソルを一度も表示していなかった
- **原因**: 旧 `GDC_CMD_CSON` (0x0B) は uPD7220 に無いコマンド。カーソル表示は CSRFORM (0x4B) の DC ビットでしか制御できない
- **対策**: `console_hw_cursor_enable()` (起動時 / V86 終了時 / `console_set_cursor`) で表示し、`console_hw_cursor_sync()` が文字列出力の末尾で論理位置へ追従させる。GDC の実状態は MCP `emu_gdc` の `m_csrform` (`8f0e7b` = 表示・2 ライン下線)

### 4-19. CPL=3 からの KAPI 呼び出し中は割り込みが止まっていた (`int 0x80` 割込みゲート)

- **現象** (〜2026-09-06): CPL=3 の `gdi_test` が最後の `kbd_getchar()` で固まり、`/api/cmd` も `/api/key` も効かない。`less` (`kbd_trygetchar` + `sys_halt`) も同じ。`/api/regs` で EFLAGS=0x004/0x084 (IF=0)、EIP は `hlt`、`tick_count` が止まり、`/api/pic` の IRR に IRQ0/1 が滞留
- **原因**: `int 0x80` は割込みゲート (0xEE) なので入口で IF=0 になるが、`int80_stub` → `ring3_syscall_dispatch` → `kapi_invoke` → `wrap_*` のどこにも `sti` が無かった。KAPI 本体は CPL=0 の `call` 経路 (IF=1) と同じ前提で書かれ、`hlt` で IRQ を待つ関数 (`kbd_getchar` / `kbd_getkey` / `sys_halt`、GUI の `gui_call(OP_WAIT)`) は IF=0 だと二度と起きない。ポーリング型のアプリは syscall の合間 (ユーザコード、IF=1) に IRQ が届くので気づきにくく、rshell 経由 (シリアルはポーリング) の検証ではさらに隠れる。M3 の「`less` 常駐中の CPL3 同時試験」はこの理由で実質検証になっていなかった疑いがある
- **対策**: `kernel/ring3_entry.asm` の `int80_stub` でカーネルセグメント復元直後に `sti`、出口 (`popad` の前) で `cli`。出口を IF=0 で通すのは、USER_DS を DS に載せてから `iretd` までに IRQ が入ると IRQ スタブが DS=KERNEL_DS にしたまま (フレームの CS が CPL=0 なので `IRETD_USER` が戻さない) CPL=3 へ帰り、最初のメモリ参照で #GP するため (gdi_test の `gfx_pixel` で実測)。`exec_run` の setjmp 復帰点にある `_enable()` は従来どおり (sys_exit の longjmp 経路)
- **検証**: `gdi_test` が rshell から約 3 秒で自動復帰してプロンプトに戻る、キーボードから `less` → `q` で終了する、`ver` が返る
- **関連 (K2、同日)**: CTRL+STOP で CPL=3 アプリを畳む経路はそれまで存在せず (V86 脱出のみ)、K2 が `ring3_abort_request` / `ring3_abort_check` を新設した。CUI からでも暴走した CPL=3 アプリを CTRL+STOP で kill できる (`ring3_abort_count` / `fault_kill_count` が +1)。KAPI 実行中に押された場合は次の syscall 入口で畳む

### 4-20. gshell 配下のアプリだけ漢字が描けない (PACKED8 判定の抜け)

- **現象** (2026-09-06、G5 後半): PEGC / Cirrus で gui_demo のテキストボックスに FEP で「日本語」を入れると、
  カーソルは 3 文字ぶん進むのに字が出ない (白い矩形だけ)。ANK は出る。gshell 自身の候補窓には漢字が出る。
- **原因**: gshell 配下のアプリは `gfx_init` を呼べない (デスクトップを消す) ので `libos32gfx_init` ではなく
  `attach_gfx` で framebuffer だけ取り直していたが、そこで `gfx_packed` (PACKED8 判定) を立てておらず、
  共有ライブラリ内の `gfx_draw_font` (漢字経路) が 4 プレーン経路に落ちていた。ANK は libos32gui が自前で
  画素を置くので気付かなかった。
- **対策**: 判定を C 側の `libos32gfx_attach()` に集約し、`libos32gfx_init` と attach の両方から呼ぶ。
  SDK ライブラリを変えたので `make external` も必要 (§4-12 と同じ罠)。
- **検証**: PEGC / Cirrus で「日本語abc」が textbox とラベルに出る。9801 (`gfxmode pc98`) は元から正常。

### 4-21. Cirrus: 窓を shutdown で畳むと単独アプリが #PF、再 init でリレーが倒れたまま

- **現象** (2026-09-06、レビュー #5 ② を入れた直後): (a) `gdi_test` (単独 CPL=3 アプリ) がクライアント面
  0104B000h で #PF。gshell 配下では通る。(b) gshell でアプリを CTRL+STOP で畳むとデスクトップが 98 側の
  黒画面に隠れる (`/api/status` の `wab_relay=0`)。
- **原因**: (a) exec がアプリ PD にクライアント面を USER で写すのは **アプリが `gfx_init` を呼ぶ前**。
  直前の shutdown が窓を畳んで `bb_base=NULL` にしていたうえ、再 init の `paging_map_phys` が共有 PT の
  PTE を supervisor で上書きして USER を消していた。(b) gshell はアプリ終了後に `gfx_init` を呼び直すが、
  グルーの init (FF82h) がリレーを 98 側へ倒すのに `s_relay_on=1` のままで `enter()` が書かなかった。
- **対策**: 窓は最初の init で一度だけ張り (supervisor + PCD)、shutdown では畳まず `bb_base` も保持。init で
  `s_relay_on=0` に戻して `enter()` に必ず書かせる。
- **検証**: `gdi_test` present_bytes=0 hw_ops=9、`ring3_guard bb` 生存、`ring3_guard cirrus` は表示面で kill、
  gui_busy → CTRL+STOP でも `wab_relay=1` 維持。

### 4-22. gshell の X4 がボタンエッジを先に見ると WM の状態機械に届かない

- **現象** (2026-09-06、`/api/mouse` で初めて実測): ドラッグの離しが失われて XOR 枠が残ったまま次の押下まで
  動き続ける、× を押すと閉じずに前面化だけ起きる、背面窓への押下がクライアントへ届く。
- **原因**: アプリの syscall 境界ポンプ (X4) が `prev_buttons` を進めてアプリへ転送していたため、X3 (WM の
  状態機械) には次のエッジが立たない。レビュー #4 ④でモーダルだけ直した問題の一般形。
- **対策**: `wm_owns_edge()` — ドラッグ中 / 窓の外 / 背面窓 / 閉じる / タイトルバーのエッジは X4 で据え置き
  (prev_buttons を進めない) → 次の X3 が拾う。前面窓のクライアント・枠だけを X4 で配る。判定順は
  `wm_button_down` と同じにする (ずれると二重配送か取りこぼし)。
- **検証**: ドラッグ → drop → 重なり再描画 → 背面クリックで前面化 → × で閉じる → チェックボックス / OK →
  デスクトップ無反応 (スクリーンショット、TASKS §7)。

### 4-23. NP21/W 実機検証の罠 (GUI 版)

- `/api/key` の `seq=SHIFT+SPACE` は **`--data-urlencode`** で送る。`-d` だと `+` が空白になり FEP が入らず、
  「Cirrus で FEP が効かない」と誤診しかけた。
- `make deploy*` は `/etc/system.cfg` を巻き戻す。`gfxmode` / `os32gui` の設定切替は `/api/reset` で検証する。
- `hotdeploy` は CUI で rshell が生きているときだけ効く (`hotdeploy_poll` は `kbd_trygetchar` から)。gshell 中や
  `ime on` 中 (`/api/key` の文字が FEP に吸われる) は先に CUI へ戻す。**GUI から CUI へ戻る経路は
  Start → "CUI mode" → 確認ダイアログ Yes だけ** (G5 で ESC の即時切替は撤去。契約 S6 / 票 W3 §4.1) —
  `tools/gui_gate.py` の `leave_gshell()` がその手順 (Start (30,H-12) → 行 3 (82, `start_row(H,3)` — 項目数から導く。§4-31) →
  Yes (410,H/2+11) → 約 6 秒待ち → `abs=off` → `rshell`)。その後 SHIFT+SPACE → `ime off` → `rshell`。
  この経路は `/etc/system.cfg` に `GUI=0` を永続化するので、GUI 自動起動へ戻すときは `os32gui`
  (その場で GUI へ入る) か cfg の `GUI=1` 書き戻しを使う。
- OS32 は NP21/W ではシームレスマウス (np2sysp `getmpos`) なので、`/api/mouse` は **`ax`/`ay`** (絶対座標
  0..65535、`ax = px*65535/639`、`ay = py*65535/(H-1)`)。`dx/dy` はバスマウス計数で効かない。検証後は `abs=off`。
- `gfx_stats` のカウンタは起動からの累計。`gdi_test` の値は起動直後の 1 回目だけが基準。
- `/api/key` の `text=` は **8 文字ずつ** 送る (`tools/gui_gate.py` の `key()`)。raw リングは 32 エントリ
  (make+break で 1 文字 2 本) しか無く、長いパスを一度に注入すると後ろが落ちる (Run... のパスが
  `/usr/bin/gui_dem` で切れ「Launch failed」と誤診しかけた)。連打も 0.3s 間隔で。
- 自己完結テストの終了は `int 0x80` で **eax=KAPI_SLOT_SYS_EXIT (84)**。eax=0 はスロット 0 = `gfx_init` で
  終了しない (ring3_guard がそれで GFX モードに入ったまま無限ループしていた)。
- NP21/W の停止は `taskkill.exe /F /IM np21x64w.exe` (os32-cycle deploy と同じ) で PM が行える。ini の編集
  ([D2]) と np21w-src の `make deploy` は停止中に。
- **エミュレータは同時に 1 人**。コーダーに「CUI で再現してよい」と hotdeploy を許可したら、PM の GUI 検証と
  混ざってゲストのファイルが差し替わり、壊れたバイナリを判定してしまった (2026-09-06)。

### 4-24. ext2: 解放系がスクラッチバッファを共有していてファイルが相互リンクする

- **現象** (2026-09-06、v1.2 G2): 15KB の `/usr/bin/v12_api_test.bin` を 21KB で上書き (hotdeploy) すると、
  新しいファイルの先頭ブロックのオフセット 12〜19 に別ファイルの間接エントリが現れ、OS32X ヘッダの
  `flags` / `entry_offset` が化けて `exec_run` が `load + 0xC639` へ飛んで #PF。`gui_demo.bin` も
  「invalid OS32X binary」になった。ホスト側のビルドは正常。
- **原因**: `ext2_free_all_blocks()` が単一間接テーブルを `ext2_g_aux` に読んで解放ループを回すが、
  `ext2_free_block()` はブロックビットマップを同じ `ext2_g_aux` に読み直す。1 本目を解放した瞬間に表が
  ビットマップに化け、以降はビットマップのバイト列をブロック番号と誤読して**他のファイルのブロックを
  片端から解放**する。解放されたブロックは次の割り当てで再配布され、ファイルが混ざる。二重間接も同型。
  12KB (直接ブロック 12 本) を超えるファイルの上書き / 削除のたびに起きていた。
- **対策**: 表を `ext2_g_blk` (単一間接) と `ext2_g_dat` (二重間接の内側) に置く (`fs/ext2_inode.c`)。
  **規則**: `ext2_g_aux` はビットマップ用。解放・割り当てを呼ぶ経路で表やデータを `g_aux` に置かない
  (§4-26 の `sys_ls` コールバックの注意と同じ根)。
- **診断の手順**: ゲストの `hexdump` でファイル先頭をホストのビルドと比べる → 差分の位置がヘッダなら
  ローダの `entry` を疑う → `/api/regs` / breakpoint で `_start` に届かないことを確認。NHD の健全性は
  `dd skip=1633` で ext2 部分を切り出して `e2fsck -fn` (読み取り専用)。**ゲストが壊した内容は Windows 側の
  コピーにしか無く、次の NHD 配備で WSL 側 (ホストの Python が書く) から丸ごと上書きされる** ので、
  配備し直せば消える (逆に、ゲストが書いた物は配備で消える — 従来どおり)。
- **検証**: 修正カーネルで 21KB のファイルを繰り返し上書き → `hexdump` がホストと一致、`v12_api_test` が
  起動する (v1.2 G2)。

---

### 4-25. GUI の「反応が遅い」は起床経路を疑う前に present と EIP を測る

- **現象** (2026-09-07、v1.2 G3): filer の右ペインで DOWN / ROLLDOWN を押すと画面が 1.3〜2 秒後に変わる。
  マウスを 2px 動かすと直後に変わるように見えた (実際は時間の一致)。gui_demo のリストは即時。
- **迷走**: 「Paint が次の周にしか出ない」「OP_WAIT が起きない」と決めつけて、アプリの二重 POLL、
  OP_INVALIDATE で Paint 即時配送、可視領域の回転、と 3 回直しても効かなかった (いずれも撤回)。
- **決め手**: (1) カーネルの `gfx_counters` (`kernel.map` の番地を `/api/mem` で読む) — 打鍵後 1.3 秒間
  `commits` / `present_bytes` が増えない = **present が 1 回も呼ばれていない**。(2) gshell の一時カウンタで
  Key 到着・OP_INVALIDATE・起床・2 回目の POLL がすべて同じ tick、present だけ 136 tick 後。
  (3) `/api/status` の `eip` を 30ms ごとに 1.6 秒サンプル → 11/11 が shlib の
  `libos32gui::draw::Painter::fill_solid` の内側。**真因**: 1 ピクセルごとに clip 4 回 + 枠 4 回の比較をする
  per-pixel ループで、右ペイン ~108k px の塗りに 1.3 秒。gui_demo は塗り面積が小さいだけだった。
- **対策**: `fill_solid` は矩形を clip と物理枠で 1 回だけ交差させ、行ごとに `write_bytes` (memset)。
  glyph / icon も行単位化。filer は選択変更で「前後の 2 行 + パス行」だけ再描画。打鍵→present が
  0.27〜0.45 秒 (HTTP 往復込み) に。PEGC / 9801 / Cirrus で同じ。
- **副産物として直した実欠陥**: gshell が Pointer を `moved` に関係なく毎周 ring に積み、
  `OP_WAIT` が眠らず 33 回/秒の空 commit をしていた (`gfx_counters` の idle 増分で発覚)。
  起床判定と配送判定が別実装で乖離し得たので `damage::deliverable_cand` に統合。
- **手順 (30 秒で決まる)**: ① `gfx_counters` を打鍵前後で読み present の有無を見る → 無ければ
  「アプリが描いていない or 起きていない」。② `/api/status` の `eip` を連続サンプルし、0x5xxxxx (アプリ) /
  0x4xxxxx (shlib) / 0x3xxxxx (gshell) / 0x1xxxxx (カーネル idle) のどこに居るかを数える。
  ③ shlib / アプリなら `i386-elf-nm -n` でシンボルに落とす。起床経路のカウンタはそのあと。

### 4-26. `sys_ls` のコールバックから FS を触ると一覧が崩れる

- **原因**: `ext2_list_dir` のコールバックの中で書き込み系の FS 操作をすると、
  共有スクラッチ `ext2_g_aux` が上書きされ、一覧の途中から別の内容を読む (§4-24 と同じ根)。
- **対策**: `ext2_list_dir` は各ブロックを先に私有バッファへコピーしてからコールバックを呼ぶ。
  呼ぶ側も、コールバック内では名前を自分のバッファに集めるだけにし、FS は戻ってから触る。
- **未監査**: FatFs / HostDrv の `list_dir` は同型の問題を抱えていないか確かめていない。

---

### 4-27. 日本語テキストの幅と切り詰め

- **現象**: `char buf[64]` のつもりで組んだ行が溢れる、切った末尾が □ になる。
- **原因**: UTF-8 の漢字・仮名は **1 文字 3 バイト**、画面では **2 桁 (16px)**。
  バイト数と桁数と文字数が全部違うので、どれを数えているのかを変数名で区別する。
- **対策**: バッファは桁数 × 3 + 1 で取る。切り詰めるときは **UTF-8 の先頭バイト境界でだけ**切る
  (続きバイト `0x80〜0xBF` の途中で切らない)。

---

### 4-28. 9MB 構成で `v86 -t` が落ちる (未解決、2026-09-10)

- **現象**: 物理 9MB (`ExMemory=8`) で `v86 -t` が
  `[ring3] #PF (CPL=3 / syscall) addr=0x00000000 EIP=0x00000000 -> kill app` で死ぬ。
  **8MB と 15MB では通る**。バックエンド (pc98 / pegc) には依存しない。
- **観測**: 落ちた後に `backing_phys` を読むと **0**。`v86_mem_setup()` は
  `pgalloc_alloc_n(V86_BACKING_PAGES)` = 159 ページの連続確保に失敗すると `-2` を返すが、
  0 番地へ飛んでいるので**呼び出し側が戻り値を見ていない疑いがある**。
  ただし `backing_phys` は起動時も 0 なので、setup に到達する前に落ちた可能性も残る
  (どちらかは切り分けていない)。
- **アイドル時の pgalloc (9MB)**: `limit_pfn=generic_end=2304`、`total_pages=1278`、
  `used_pages=256` (shlib 帯)。空きは 1022 ページあり、159 は十分に見える。
  アプリ実行中は `exec_child_claim` が a=[0x500000,0x7BD000) と b=[0x8BD000,0x8FE000) を
  取り、残る A/B 穴は [0x7BD000, 0x8BD000) の 256 ページ。**なぜここから 159 が
  取れないのかは未解明**。
- **撤回した推測**: 「A/B 穴が 0x800000 (アプリ固有 PDE の境界) をまたぐのが原因」と
  一度書いたが筋が悪い。V86 のバッキングは**物理**ページで、写像先はゲスト線形
  0x1000-0x9FFFF = PDE 0 (`kernel/v86_mem.c` の `paging_map_range`)。バッキングの
  物理位置が PDE 1 の境界をまたぐかどうかはこの写像に影響しない。
- **窓の撤去とは無関係と考えている**が、旧カーネルでの再現はしていない。
- **次に見るべき所**: 失敗時の `pgalloc_free_pages()` / `used_pages` と、
  `alloc_n_pfn` が実際に走査した範囲。`v86` コマンドが CPL=0 か CPL=3 かで
  `exec` のレイアウトが変わるので、そこも確認する。
- **影響**: PEGC GUI の下限として 9MB を採る場合にぶつかる。CUI 8MB と 15MB は無事。

---

### 4-29. NHD 満杯を `make deploy-nhd` が黙って通していた (2026-09-10、修正済み)

- **現象**: 配備は「完了! 183 ファイル」「Done! (199.9 MB copied)」と出て **exit 0**。
  だが NHD の `/boot/vmkernel.lz4` が **446,464 B に切り詰められて**いて、
  手元の成果物 (448,812 B) と一致しない。ゲストは古いカーネルで動き続ける。
- **本当のエラー**: 出力の途中に 1 行だけ出ている。grep しないと流れる。
  `Error: vmkernel.lz4 -> /boot/vmkernel.lz4: cp: error writing ...: No space left on device`
- **`df` が嘘をつく**: 「69M 空き / 63% 使用」と出るのに ENOSPC。
  `e2fsck -fn` で `Free blocks count wrong (80315, counted=2)` — スーパーブロックの
  空きブロック数が壊れていた。修復後の実数は **203,931 / 203,932 ブロック使用**。
- **満杯の原因**: NHD ルート直下にホスト側のディスクイメージが入っていた
  (`dos5hd.nhd` / `dos5hdmaster.nhd` で 82MB、`Ys*.D88/NFD`・`dos5*.fdi`・
  `fd98_2hd.img`・`os32_serial_log.txt` で約 10MB)。原本は `C:\os32` にあり
  ゲストからは `/host` で見えるので、NHD 側は重複。削除して 92MB 空けた。
- **対処**: `sudo losetup -f --show --offset 836096 build/nhd/os32.nhd` で
  ループを張り `sudo e2fsck -fy <loop>` で修復 → 不要ファイルを削除 → 再配備。
- **教訓**: 配備の成否を「完了/Done の文言」で判断しない ([V4])。
  **必ずゲストの `ls -l /boot/vmkernel.lz4` と手元の `stat -c%s` を突き合わせる**。
  `os32-cycle deploy` はこの照合を持つが、`make deploy-nhd` を直接叩くと素通りする。
- **修正済み (2026-09-10)**: `nhd_deploy.py` の `do_sync` / `do_sync_from_hostdrv` が
  失敗を数えるようにし、1 件でも失敗したら `False` を返す (末尾は「完了!」ではなく
  「失敗! N ファイルをコピーできなかった」)。`sync-from-hostdrv` は戻り値すら
  見ていなかったので `sys.exit(1)` を足した。あわせて**失敗した宛先を消す**
  (`remove_partial`) — `cp` は書き込み前に宛先を切り詰めるので、残すとゲストが
  「存在するが壊れた成果物」を掴む。消えていれば NOT FOUND で失敗が見える。
  回帰は `tools/tests/test_nhd_deploy_failure.py` (`make check-tools-host` に登録)。
  **それでもサイズ照合はやめない** — 失敗の形は ENOSPC だけではない。

### 4-30. `/fd0` が hd0 に化けて ext2 を二重マウントし、スーパーブロックを巻き戻していた (2026-09-10、修正済み)

- **現象**: きれいな ext2 に 106KB のファイルを 1 個 `cp` して `sync` するだけで、
  ホスト側 `e2fsck -fn` が必ず `Free blocks count wrong for group #0 (4097, counted=3991)` /
  `Free inodes count wrong for group #0 (1761, counted=1760)`。ずれはちょうど
  ファイル 1 個分。**強制終了とは無関係** (`CloseMainWindow` の通常終了でも同じ)。
  ビットマップ側は常に正しい。
- **切り分け**: `mounts[0].fs_ctx` の `free_blocks_count` (ctx+52) は
  cp 前 91386 → cp 後 91280 (−106) と**メモリ上は正しい**。一方 `sync` 後の
  イメージ `836096+1024+12` は 91386 のまま。**ディスク上だけが巻き戻る**。
- **踏んだ落とし穴**: `ext2_write_super_raw` にブレークを置いたら 0 ヒットだったので
  「sync がスーパーブロックを書いていない」と判断した。**誤り** —
  `ext2_write_super_raw` は `ext2_sync` に**インライン展開**されていた
  (`i386-elf-objdump -d` で確認)。書き込み自体は走っていた。
  **static でない関数でもインライン化される。ブレーク 0 ヒットを未実行の証拠にしない。**
- **真因**: `vfs_sync()` から `ext2_vfs_sync` が **2 回**呼ばれていた。マウント表を実機
  メモリから読むと `mounts[1] prefix=/fd0 dev=fd0` の `Ext2Ctx` が `base_lba=1632`、
  `dev` ポインタまで `/` と同一。`vfs_mount()` は
  `ops->mount((dev_type << 8) | dev_id)` と種別を上位バイトに載せて渡すが、
  ext2 は下位バイトしか見ていなかったので `fd0` (=`0x100`) が
  `ide_drive_present(0x100 & 3)` → hd0、`ext2_dev_for` → `"hd0"`、
  `ext2_find_partition` → `drive_info[0]` と**すべて hd0 に化けていた**。
  ブート時の自動マウントループ (`kernel/kernel.c`) が root 以外の全ブロック
  デバイスに ext2 を試すので、`/fd0` として 2 つ目の `Ext2Ctx` が必ずできる。
  `vfs_sync()` は全マウントを回すため、`mounts[0]` が正しい空き数を書いた直後に
  `mounts[1]` が**マウント時点のスナップショット**を同じセクタへ書き戻す。
  ビットマップはこの経路で触らないので真値が残り、「counted=」不一致になる。
- **修正**: `fs/vfs.h` に `VFS_DEV_*` と `VFS_MOUNT_DEV_ENCODE/TYPE/ID` を公開し、
  `ext2_vfs_mount` は `VFS_DEV_HD` 以外を ext2 本体に届く前に拒否。
  `iso9660_mount` も同じ取りこぼし (`'0' + (char)dev_id`) があったので CD 限定に。
  さらに `vfs_mount()` が同じ (ops, 種別, unit) の二重マウントを `VFS_ERR_EXIST` で
  断る網をクラスごと張った。回帰は `tools/tests/test_vfs_mount_dev.py`
  (`make check-vfs-mount-dev-host`)、経緯は `tools/tests/vfs_mount_dev_tdd.md`。
- **実機確認**: 新カーネル配備後、マウント表は `/` と `/host` のみ (`/fd0` が消えた)。
  438KB のファイルを `cp` + `sync` → 通常終了 → `e2fsck -fn` が **RC=0 クリーン**。
  コピーは md5 一致。
- **教訓**: 「FS ドライバは下位バイトしか見ないので互換」というコメントが
  `fs/vfs.c` にそのまま書いてあった。**呼び出し側が広げたエンコードは、
  受け側全部を数えて確かめる**。片方が無視すると、別デバイスが同じ実体に化ける。

### 4-31. `gui_gate.py` で GUI を叩くときの 2 つの罠 (2026-09-10、修正済み)

- **rshell を抜けてから `/api/key` の text を打つ。** rshell は `kbd_trygetchar` の
  生読みで、入力が途切れるたびに 1 コマンドとして実行する。4 文字ずつ送る
  `key(text="os32gui")` は `os32` / `gui` という別々のコマンドになり、GUI には入らない
  (画面に `os32: command not found` / `gui: command not found` が並ぶ)。先に
  `key(seq="ESC")` で `[Remote shell closed]` を出してから `enter_gshell()`。
  `leave_gshell()` は末尾で `rshell` を打って復旧するので、**台本側で二重に打たない**
  (GUI 内で打つとターミナルが rshell を起動し、以後の打鍵を全部食う)。
- **Start メニューの行座標は項目数から導く。** メニューはタスクバーから上へ伸びるので、
  v1.3 (T5a) で "Display fixture" が足されて 5 → 6 行になった時点で全行が 18px 上がり、
  5 行前提の固定値 `H-107+18r` は行 r が r+1 に当たっていた。「CUI mode」(r=3) の
  クリックが **Shut Down** に当たり、確認 Yes でゲストが `System halted` になった
  (リセットで復旧、NHD は無傷)。`gui_gate.py` は `startmenu.rs` の `ROOT_ITEMS` /
  `ITEM_H` / `BORDER` と `taskbar.rs` の `TASKBAR_H` から計算する形に直した。
  **項目を足したら `START_MENU_ITEMS` も更新する。**
- 観測は `gui_bench` の `CLICK n` (text VRAM) が便利。`on_raw` で `Button` を数えるので、
  WM がアプリへ配ったかそのものが見える。1 クリック = +2 (押下+解放)。

---

## §5. デバッグ道具箱

### カーネル内デバッグ出力

| 関数 | 用途 | 出力先 |
|------|------|--------|
| `kprintf()` | カーネル内デバッグメッセージ | テキストVRAM (コンソール) |
| `serial_printf()` | シリアル経由デバッグ出力 | RS-232C → ホスト側ターミナル |

シリアル出力はコンソールを汚さないため、画面描画に影響するバグの調査に特に有用。

### NP21/W リモート実行 (HTTP API)

NP21/W (ai-debug フォーク) は内蔵のデバッグ HTTP サーバを持つ。`np21x64w.ini` で
`aidebug=true` / `aidbport=8025` を設定すると、OS32 の rshell と HTTP で対話できる。
外部の中継プロセスは不要。

```bash
# コマンド実行 (行単位。生の打鍵を読む相手には届かない)
curl -X POST http://127.0.0.1:8025/api/cmd --data-binary "ver"

# キーイベントの注入 (FEP 変換、エディタ、ゲームなど)
curl -X POST http://127.0.0.1:8025/api/key -d "seq=SPACE"
curl -X POST http://127.0.0.1:8025/api/key --data-urlencode "seq=SHIFT+SPACE"   # FEP on/off

# マウス (OS32 はシームレス絶対座標)。ax = px*65535/639, ay = py*65535/(H-1)
curl -X POST http://127.0.0.1:8025/api/mouse -d "ax=32818&ay=32851&btn=1&hold=80"

# 画面テキスト (UTF-8) — 画面判定はこちらの方が速い
curl -s http://127.0.0.1:8025/api/tvram

# スクリーンショット取得
curl -s http://127.0.0.1:8025/api/screenshot > screenshot.png
```

- `+` を含む `seq` は `--data-urlencode` を使う (`-d` だと `+` が空白になる)。
- `btn=1` / `btn=0` でドラッグ、`abs=off` で人間にマウスを返す。
- `-m` は短くしない ([V3]、最低 15 秒、長いプログラムは 60 秒以上)。
- GUI 検証の罠は §4-23 にまとめてある。

> WSL からの `127.0.0.1:8025` が届かない環境 (NAT モード + ファイアウォール) では、
> Windows 側の curl (`/mnt/c/Windows/System32/curl.exe`) を使うか、
> WSL をミラーモード (`.wslconfig` の `networkingMode=mirrored`) にする。
> レジスタ・メモリ・逆アセンブル・ブレークポイントまで要るときは
> `tools/np21w_mcp/` の MCP サーバを使う。

### 有用なゲスト側コマンド

| コマンド | 用途 |
|---------|------|
| `ver` | ビルドタイムスタンプ・バージョン確認 |
| `mem` | メモリマップ・使用量確認 |
| `ls -l` | ファイル一覧 (サイズ・inode 確認) |
| `cat /etc/profile` | 起動設定の確認 |

### ビルド→デプロイ→テストの標準サイクル

| 変更対象 | 手順 | NP21/W再起動 |
|----------|------|:---:|
| プログラムのみ (HostDrv実行) | `make all` → `make deploy` | 不要 |
| プログラムのみ (ホット) | `make hotdeploy FILE=<path>` | 不要 |
| カーネル / ブートFS | NP21/W停止 → `make all` → `make deploy && make deploy-kernel` → `np21w_restart.py` | **必要** |

```bash
# カーネル変更時のフルサイクル例
taskkill.exe /F /IM np21x64w.exe
make all && make deploy && make deploy-kernel
WIN_NP21W_DIR='C:\...\np21w' python3 tools/np21w_restart.py
curl -X POST http://127.0.0.1:8025/api/cmd --data-binary "ver"   # Build タイムスタンプ確認
```

---

## §6. AI との協調デバッグ

### AI に期待する役割

| 段階 | AI の役割 | 人間の役割 |
|------|----------|-----------|
| 仮説立案 | コードベースを分析し、可能性のある原因を網羅的に列挙 | 仮説の妥当性を評価し、優先順位を承認 |
| 検証コード作成 | 最小限のデバッグコードを提案 | ハードウェア制約違反がないかレビュー |
| ビルド・デプロイ | ワークフローを実行 | NP21/W 上の動作を目視確認 |
| 結果分析 | 出力を解析し、仮説の正否を判断 | 最終的な修正方針を決定 |

### AI が守るべきルール

1. **コード変更前に仮説を提示**: いきなりコードを修正しない
2. **バイナリ反映を最初に疑う**: デバッグ出力が出ない → まず §2 のチェックリストを実行
3. **3回リトライで報告**: 3イテレーション以内に解決しなければ立ち止まる
4. **ハードウェア制約の遵守**: §3 (POLICY_DEV.md) の制約に違反するコードは絶対に提案しない
5. **教訓集を参照**: 新しい問題に直面したら、まず §4 に類似パターンがないか確認する

---

*OS32 Debug Policy — Created: 2026-04-18*
