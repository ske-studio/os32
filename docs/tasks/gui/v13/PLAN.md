# GUI v1.3 — 着手計画

状態: **T4独立モデル受入済み・CUI統合契約未凍結**。
PM: Hermes。低レイヤ担当: claude5 (モデル: claude-opus-5)。GUI/ユーザーランド担当: Codex。
T0/T1設計とT4実装・ホスト検証を実施済み。CUI統合・ゲスト試験は未実施。

親計画: [ROADMAP](../../../ROADMAP.md)。現行契約: [v1.2 CONTRACTS](../v12/CONTRACTS.md)。
設定レジストリの仕様案は [settings DESIGN](../../settings/DESIGN.md) を正典とし、ここには進行条件だけを置く。

## 1. 範囲と進め方

v1.3 の目標は GUI 上の CUI 実行、入出力抽象化、設定レジストリ。
最初から対話端末の完成を主張せず、有限入力・終了後出力表示の縦切りから安全性を確かめる。
既存の single-foreground-app、X3/X4 の制約は、新契約を承認するまで変更しない。
KAPI の追加が必要なら予約番号を KAPI_SPEC §3-2 と調停する。v43 を無断で使用しない。

## 2. 確認した接点とブロッカー

### ターミナル

- `docs/tasks/gui/v12/CONTRACTS.md` S1/S2: 外部アプリは1本。handler/X3/X4から直接execは禁止。
- `fs/fd_redirect.c:166-179,197-209`: バッファ入力は空でEOF、出力は固定容量のshort write。対話ストリームではない。
- 調査指摘: GUI中はraw入力で、既存CUIのcooked待機へ直接は届かない (`drivers/kbd.c:361-408`)。
- 調査指摘: FD捕捉だけでは直接コンソール出力を拾えない (`kernel/console.c:214-346`)。
- 調査指摘: lconsoleは80×25・原点固定描画。窓内surface/clipへそのまま接続できない
  (`userland/lib/gfx/lconsole.h:10-27`, `userland/lib/gfx/text/lconsole.c:297-360`)。

最小候補は gshell 常駐側で結果を表示し、top-levelからCUIを1本実行する構成。
ROADMAPの「terminal app」との差異は未決であり、外部terminalから外部CUIをネスト起動する実装は発注しない。
対話入力、実行中描画、FEPには安全な待機・サービス点と終了契約を別途定義する。

### 設定レジストリ

- `kapi/kapi_db.c:187-216`: 現行openは `sqlite3_open` と `journal_mode=DELETE`。真のreadonly/no-create契約を提供していない。
- `kapi/kapi_db.c:254-281`: execはSQLを固定バッファへコピーし、先頭statementだけprepare/stepする。
  `BEGIN; UPDATE; COMMIT` を1回渡す設計にしない。
- 調査指摘: 通常NHD配備は対象を上書きする (`tools/nhd_deploy.py:642-655`)。
  マスタを通常配備へ登録すると、ユーザー設定を保持する要求と衝突する。
- 調査指摘: bind API不在とSQLサイズ制限により4KB blob書込み方式が未成立。
- 調査指摘: SQLite VFSのsyncエラー伝播、接続所有権、全接続共通SHM、FEPとのプール共存を追加確認する。

「KAPI追加なし」は未検証の目標。非破壊性を優先し、S0で可否を決定する。

## 3. 作業票と担当予定

| 票 | 担当予定 | 成果物・許可範囲 | 完了条件 |
|---|---|---|---|
| T0 待機・所有権契約 | claude5 / opus5 | 読取調査と契約案。kernel/exec/FD/kbdの接点を追う。コード変更不可 | 常駐型/外部型比較、blocking read中のサービス点、出力捕捉、CTRL+STOP/fault/起動失敗時の回収を根拠付きで示す |
| T1 端末表示設計 | Codex | T0と並行でlconsole/gshell調査。コード変更不可 | セル・UTF-8・窓内clip・再露出の設計と、有限入出力の最小縦切りを定義する |
| S0 非破壊DB契約 | claude5 / opus5、PM決裁 | settings DESIGNの改訂案。配備・KAPI変更不可 | 下記の非破壊/transaction/型/媒体契約を凍結し、後続基盤票を分離する |
| R0 独立レビュー | 実装・設計担当と別のエージェント | 読取専用レビュー | 未解決の重大指摘を列挙し、解決前は実装ゲートを通さない |
| V0 検証設計 | PM + 専任検証セッション | ホスト/ゲスト試験の対象と証跡形式 | 下記受入条件を再現可能な台本に落とす。実行は成果物確定後 |

T0 (claude-opus-5) とT1 (Codex) の読み取り専用設計を受領済み。
PMレビューのR1〜R5をT0修正版が反映。統合の安全条件が未解決のため契約凍結は保留。S0は未発注。
判定と次のゲートは [REVIEW_T0_T1.md](REVIEW_T0_T1.md) を参照。
メモリのホスト測定は [MEMORY_BUDGET.md](MEMORY_BUDGET.md)。ゲスト実行時の空きは未測定。
T0とS0を同一エージェントへ同時発注しない。共有ファイルの編集はPMが直列化する。

## 4. S0で凍結する事項

1. missing/破損/異版/readonlyのopen挙動。missingは仮想ハンドル等で扱い、新規DBを自動生成しない。
2. readonly/no-createを既存APIで保証できるか。query_onlyや事前statだけを非変更保証の代わりにしない。
3. BEGINは明示呼出しに一本化する案を検討。set失敗後のcommit、close時rollback、接続直列化を定義。
4. int32、scope/key/text/blob上限、UTF-8、SQL引用、cap不足、エラーと既定値、列挙再入の契約。
5. 4KB blobの方式: 制限変更、分割書込み、bind API追加を比較し、黙って切り捨てない。
6. 媒体マスタ、新規インストール、通常更新、明示リカバリを分離。通常deployは既存/欠損DBとも変更しない。
7. sync/rollback journalの耐障害性。MEMORY journalをクラッシュ回復の代替にしない。
8. FEPとの共有プール、共通SHM、子終了時のDB cleanupを含む寿命。必要なら独立した基盤修正票を切る。

## 5. 受入試験候補 (未実行)

### ターミナル

- 有限stdinのEOF、stdout/stderr分離、容量超過の通知、次コマンドへのredirect残留なし。
- 正常終了、起動失敗、fault、CTRL+STOPでdesktop・所有資源を回収する。
- 完全統合段階ではprintf/kprintf/直接コンソール出力を窓へ集約し、TVRAM漏れなし。
- UTF-8分割入力、右端の日本語、CR/LF/BS、スクロール、窓再露出、窓外描画なし。
- 対話段階ではblocking read/getkey、ASCII/矢印/FEP、未入力時の待機、Quit/abortを確認。
- PC98/PEGC/Cirrusの回帰とGFX子終了後のdesktop復元。

### 設定

- missing/空/破損/異版/開けないDBでfallbackし、元DBとjournalを変更しない。
- readonlyで全更新を拒否。hot journalを含む失敗時も非変更条件を検証。
- commit/reopen、rollback、close、途中失敗、容量不足で期待する永続化と回復を確認。
- 型・長さ境界、空blob、最大blob、SQL引用、cap不足、先頭行、enum再入。
- FEP変換/学習との交互実行と異常終了後のpool/FD/DB slot回収。
- 通常配備で設定非変更。recover失敗で旧DB維持、成功時もsystem.cfg/boot領域は不変。

破損・強制終了試験は承認済みの使い捨てイメージのみ。通常NHDへの障害注入は禁止。
ビルド・配備・ゲスト操作は専任検証セッションに集約し、[D1]〜[D3]の承認を別途取得する。

## 6. 進捗

- Hermes調査サブエージェントがターミナルと設定を読み取り専用で並行監査。
- PMがsingle-foreground契約、buffer redirect、DB open/execの主要指摘を現行ファイルと照合。
- 本計画を作成。契約凍結・コード実装・ゲスト試験は未実施。
- T1: Codexが終了コード0で設計報告を提出。ローカル報告は `/tmp/os32-v13-t1-design.txt`
  (一時成果物、仕様の正典ではない)。常駐結果表示UIを推奨。対話入力・完全捕捉は未解決。
- T1の追加論点: stdout/stderrの相互順序は別バッファから復元不能。満杯と出力欠落は保存長だけでは
  区別不能。stdio終了時flush、親redirectの保護、終了理由の識別をT0と合わせて確認する。
- T0: 起動コマンドは `claude`、正確なモデルIDは `claude-opus-5` と確認。調査は35ターン上限で停止したが、
  同一セッションを再開して最終報告を受領 (exit 0)。原報告は `/tmp/os32-v13-t0-design-final.txt`。
- PMレビュー: 戻り値からの原因断定、redirect消失後の長さ、pipeのメモリ負担、外部直列型の扱い等を差し戻し。
  常駐型の有限入出力案は方向性として採用するが、T2/T3の実装発注は未承認。
- 設定実装S1/S2はS0完了まで保留。T1も設計提出段階であり、受入・契約凍結・実装完了ではない。
- T0修正を同一claude-opus-5セッションへ発注。R1〜R5の修正と独立モデル試験票の提案に限定し、実装は禁止。
- PMが `make gshell` を実行し成功。静的メモリ配置とT1容量案を測定・記録。NHD未配備。
- T0修正版を受領 (exit 0、`/tmp/os32-v13-t0-revised.txt`)。R1〜R5の誤記・断定は撤回済みと確認。
  バッファ方式/実行時容量、監査対象の同一性、対話待機の安全条件は未解決。
- 統合から独立した [T4 セル/UTF-8/clipモデル](TASK_T4_MODEL.md) の作業票を発行。
  T4は `userland/libos32term/` に実装し受入済み。[PM判定](REVIEW_T4.md)を参照。
  gshell+CUI統合は未着手。
- CodexのTAB修正・L1明記・Cellホスト実寸確認を受領し、独立再レビューで受入可。
  PMが `build/sdk.mk` に `check-term-model` を追加し `check` の依存へ登録。
  修正後、新ターゲットを含む `make check` 全体（モデル44試験）、fmt、差分チェックが成功。
  manifest検査はCargo incrementalの `.bin` も未配備一覧へ列挙する(非エラー)。
- PMが[guestターゲットの限定リンク検査](CROSS_LINK_T4.md)を実施し成功。
  Grid::clipのi64除算ヘルパ解決とCellのターゲット実寸8B/align4Bを確認。ゲスト実行はなし。
- CodexのT5表示設計を受領。[T5表示票](TASK_T5_DISPLAY.md)に方向性と実装前ゲートを記録。
  独立表示アプリT5aを先行し、常駐接続T5bは後段へ分離する。実装未発注、CUI起動を含めない。
- T5aゲートの独立調査を受領し、[PM照合](REVIEW_T5_GATES.md)を記録。
  glyphビット順・JIS各バイト検査・utf8_prog.oの接続を確認。
  純粋描画アダプタを `userland/libos32term_render/` に先行実装する配置を決定。
- [T5R作業票](TASK_T5_RENDER.md)で文字分類・clip・失敗時副作用・人工glyph試験を確定し、Codexへ実装発注。
  T5Rは対象再実装後の独立レビューとPM再実行を通過し受入済み。
  check-term-renderをmake checkへ登録し全体成功。[受入記録](REVIEW_T5_RENDER.md)。
- [T5aアプリ票](TASK_T5A_APP.md)を発行しCodexへ新規アプリ内限定で実装発注。
  既存モデル/rendererの事前ハッシュは `/tmp/os32-t5a-source-baseline.json`。
  workspace/build/deploy登録とゲスト最終リンクは提出後PMが実施。配備・ゲスト操作は未承認。
