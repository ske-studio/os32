# TASK_KAPI_DATA_FIELDS — KAPI のデータ欄 (sbrk_heap_limit / shm_base) が関数追加のたびにずれ、旧バイナリが黙って壊れる

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-23) / 状態: **起票 (未着手)**。カーネル層 (KAPI / exec) の既知の欠陥なので POLICY_DEV §1 に沿って新機能より先に扱う。
> 出所: キーボード修正 (bda95fa / f924275、KAPI v62) の実装レビュー。ラリー 1 で Codex が blocker、Opus が非 blocker と判定が分かれ、ラリー 2 で**両者とも「この commit 固有ではない構造問題、別票 (b)」で一致**。

## 事実

- 生成器はデータ欄を関数表の直後に置く (`exec/exec.c:125`、`tools/check_kapi_version.py:71`)。関数を 1 つ足すたびに `sbrk_heap_limit` / `shm_base` のオフセットが 4 バイト動く (KAPI_SPEC の版表: v50 0x348 → … → v61 0x39C/0x3A0 → v62 0x3A0/0x3A4)。
- exec の検査は `min_api_ver > KAPI_VERSION` だけ (`exec/exec.c:1446`)。OS32X ヘッダにビルド時のデータ欄配置を示す欄は無い。
- **反例 (到達可能)**: v61 でビルドした `_sbrk` を使う C アプリを v62 で起動 → 0x39C (= v62 の `kbd_diag` のトランポリン番地、< 0x500000) をヒープ上限と読む → `sdk/crt/syscalls.c:140` で malloc が全部 ENOMEM。`shm_base` はヒープ上限に化け、libos32db の結果読みと GUI スロットが狂う。**逆方向も到達可能**: `make deploy` + `hsync` だけで `deploy-kernel` をしないと、v62 ビルドの userland が v61 カーネルに載る。`min_api_ver` が低いまま配置に依存するアプリの例: `gshell` (50、Rust の shm_base)、`db_v50_test` (shm_base)、`heap_test` (sbrk_heap_limit)。

## 当面の運用 (現行の [ABI3])

KAPI を上げたら `make clean` → `make all` → `make external` → **カーネルとユーザーランドを同時に全配備** (HostDrv だけ・旧バイナリの持ち込みは守備範囲外)。

## 最小案 (Opus 案、レビュー前)

1. mkos32x が OS32X ヘッダに「データ欄のオフセット (= 8 + 4 × 関数数)」を書く。
2. exec はそれがカーネルの値と違えば `rebuild required` で断る (load_addr の照合と同じ作法)。
3. 欄の無い旧ヘッダは一度だけ全部断る (全再ビルド)。
4. 恒久策 (データ欄を固定オフセットへ: 関数表の容量を予約してその後ろ、またはアクセサ KAPI) はその後。
