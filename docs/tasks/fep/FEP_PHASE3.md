# FEP Phase 3: カーネル入力フックとイベントルーティング

## 目的
実機上で任意の文字列入力時にシステムがFEPによる変換を割り込ませるための入力パイプラインを構築する。既存の `programs/skk` の知見を活用し、まずは部分的なフックとイベントルーティングの仕組みを固める。

## 前提条件
- Phase 2 の検索エンジンがテストアプリで動作確認済みであること
- 既存キーボードドライバ: `drivers/kbd.c`
  - IRQ1割り込みでスキャンコード→ASCII変換をリングバッファに格納
  - `kbd_shift_state` でShift/Ctrl/Caps/Kana/Grph状態を管理
  - KernelAPI: `kbd_getchar`, `kbd_getkey`, `kbd_trygetkey`, `kbd_get_modifiers`
- 既存SKKステートマシン: `programs/skk/skk_state.c`
  - `skk_process_key(u8 key, u32 modifier, SKK_STATE *, char *out)` の構造が参考
  - IMEトグルは `key == 0x00` (Ctrl+Space) で実装
  - ローマ字→かな変換エンジン (`skk_rom_kana.c`) を再利用可能

## タスクリスト

### Phase 3a: キーボードフック機構の構築
- [ ] `programs/skk` のキーフック・ステートマシン構造の解析
  - `skk_process_key()` のディスパッチ構造をFEP用に一般化
  - `skk_rom_kana.c` のローマ字→ひらがな変換エンジンをFEPエンジンに移植（または共通ライブラリ化）
- [ ] 入力切り替えキーの検知ロジック実装
  - **デフォルト**: `Shift + Space`（PC-98スキャンコード: KEY_SPACE=0x34 + SHIFT_SHIFT フラグ）
  - `kbd.c` のIRQ内部または `kbd_getkey` 返却値レベルでの判定方法の検討
  - 将来の変更に対応するため、トグルキーを定数化（定義ファイルに分離）
- [ ] FEPモードフラグの管理方法（グローバル変数、KernelAPI公開等）の決定

### Phase 3b: 入力モード管理とルーティング
- [ ] Google日本語入力の挙動をベースとしたステートマシン設計
  - **入力状態**: 直接入力 / ひらがな入力 / 変換中 / 候補選択中
  - **キーバインド**:
    - `Space`: 変換実行 / 次候補
    - `Enter`: 確定
    - `Escape`: キャンセル（未確定文字破棄）
    - `Backspace`: 未確定文字の削除
    - `Tab`: 予測変換の展開（将来）
    - `←` `→`: 変換範囲の変更（将来の連文節対応）
- [ ] FEPモードON時の入力遮断処理
  - `kbd_getchar` / `kbd_getkey` からの返り値をFEPエンジンが横取りし、通常のキーバッファへの伝搬を遮断
  - テストアプリ段階: アプリ内ループでの入力横取り
  - 常駐化段階: カーネル側でのフック（`kbd_irq_handler` の拡張、またはコールバックコード挿入）
- [ ] FEP内で確定された文字列（UTF-8）をカーネルの標準キーバッファなどにインジェクト（挿入）するAPIの整備
  - 方式案A: 新規KernelAPI `kbd_inject_string(const char *utf8)` の追加
  - 方式案B: アプリ側のバッファに直接書き込み（非透過的; テスト用）
  - 方式の決定はユーザーとの相談の上で行う

### Phase 3c: ローマ字→ひらがな変換の統合
- [ ] `skk_rom_kana.c` のローマ字テーブルをFEP用に調整（SKK固有のルールの除去）
- [ ] 「ん」の確定タイミングの処理（nn / n+子音 / n+母音の判定）
- [ ] 小文字「っ」の自動変換（子音の重複: tt→っt 等）

## 成果物
- FEPステートマシン実装（`lib/fep_state.c` または `programs/fep_test` 内）
- ローマ字→ひらがな変換モジュール（`lib/fep_romaji.c` または skk_rom_kana の共通化）
- KernelAPI拡張（必要に応じて `kapi.json` に追記）

## 設計上の注意点
- **PC-98固有のキーコード**: Shift+Space のスキャンコードは、`kbd_irq_handler` がShiftフラグのみを更新し、Spaceのメイクコードとの組み合わせで検知する必要がある。ASCII=0x20 かつ `kbd_shift_state & SHIFT_SHIFT` で判定可能。
- **変換キー(XFER)**: PC-98には専用の「変換」キー(KEY_XFER=0x35)が存在する。将来的にはこのキーにも機能を割り当て可能にする。
- **シングルタスクの制約**: OS32はシングルタスクOSのため、FEPモードON時はアプリケーションのメインループが停止する。入力ループはFEPエンジン側が制御する設計。
