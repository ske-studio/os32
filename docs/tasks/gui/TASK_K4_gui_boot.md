# K4: `/etc/system.cfg`、テキスト GDC 制御、`os32gui` コマンド

> 発行: PM (2026-09-05) / レーン: K / 前提: なし (今すぐ着手可)。gshell の実体は W1
> 親: [TASKS.md](TASKS.md) / ロードマップ: `docs/ROADMAP.md` v1.1「CUI → GUI 切替フロー」
> 排他: `kernel/kernel.c` (シェル起動ループ)、`kernel/console.c`、`userland/shell/**`

## ゴール

ROADMAP の切替フローをカーネルとシェルに実装する。gshell が無くても
(`/bin/gshell.bin` が無い場合) CUI に落ちて起動できること。

## 作業

1. **`/etc/system.cfg` 解析**: `kernel.c` のシェル起動ループで `GUI=0/1` を読む
   (行指向、`KEY=VALUE`、`kstr*` のみ)。1 なら `/bin/gshell.bin` を Level 0 として起動、
   無ければ警告を出して `shell.bin`。gshell が exit で戻ったら cfg を再読取して分岐
   (ROADMAP の「GUI終了プロセス」5〜6)。
2. **テキスト GDC 制御**: `console_text_gdc_stop()` / `start()` を `console.c` に。
   gshell が `gfx_init` (H1 の `enter`) の前後で呼ぶ KAPI として **K1 の v41 に相乗り**
   させるか、gshell が CPL=0 なので内部関数を直接呼ぶかは K が決める (後者なら KAPI 不要)。
   9801 バックエンドはクローム描画に TVRAM を使える (DESIGN R4) ので、STOP するのは
   `GFX_CAP_TEXT_OVERLAY` が 0 のバックエンドのときだけ。判定は gshell 側 (W1)。
3. **`os32gui` コマンド** (`userland/shell/cmd_sys.c`): `system.cfg` に `GUI=1` を書いて
   `sys_reboot()`。`os32gui off` で `GUI=0` を書くだけ (再起動しない)。
4. **ハング復旧の確認**: FDD ブート → `mount /hd0` → `edit /hd0/etc/system.cfg` の
   手順が今の FDD イメージで通ることを 1 回確かめて票に残す。

## 完了条件 (ゲート G3 の起動部分)

- `os32gui` → 再起動 → gshell が上がる (W1 完了後)。W1 前は「gshell.bin が無い」警告で
  CUI に落ちることを確認。
- gshell から「CUI で再起動」→ `shell.bin` が上がる。
- `GUI=1` のまま gshell がクラッシュ (`fault_kill`) しても CUI ではなく gshell を再起動する
  ループに入らないこと: 連続 3 回失敗したら CUI に落とす保険を入れる。
