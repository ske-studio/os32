# TASK_KAPI_DATA_FIELDS — KAPI のデータ欄 (sbrk_heap_limit / shm_base) が関数追加のたびにずれ、旧バイナリが黙って壊れる

> 発行: PM (Claude Code `claude-opus-5-5`、2026-09-23) / 状態: **方針レビュー (Codex + Opus、ラリー 1)** — ユーザー指示「別票を着手」(2026-09-24)。カーネル層 (KAPI / exec) の既知の欠陥なので POLICY_DEV §1 に沿って新機能より先に扱う。
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

## 方針 (レビュー対象) — まず検出、恒久策は同じ票の段 2

**段 1 (検出、KAPI v63)**
1. `sdk/gen_kapi.py` が `KAPI_DATA_FIELDS_OFF` (= 8 + 4 × 関数数、今のデータ欄の先頭オフセット) を生成物に出す。`tools/check_kapi_version.py` が一致を検査する。
2. OS32X ヘッダを **v3** にし、末尾に `kapi_data_off` (u32) を追記 (`OS32X_HDR_V3_SIZE`)。`tools/mkos32x.py` は SDK の生成物からこの値を焼く (C / Rust / 外部 repo のビルドはすべて mkos32x を通る — 確認する)。
3. exec (`exec/exec.c:1446` 付近) は、**ヘッダ v3 未満、または `kapi_data_off` が現在のカーネルの値と違えば**「`rebuild required (KAPI data layout)`」で断る。**常駐シェルと共有ライブラリ (`libos32gui.shlib`) にも同じ検査** (shlib のローダ側)。旧バイナリは一度だけ全部断られる = `make clean` → `make all` → `make external` → 全配備が要る (ROADMAP / 08_build に明記)。
4. hsync は現行の「KAPI v53 未満のカーネルを断る」に加え、`/host` の名札の KAPI 版とカーネルの版が違えば警告する (配置違いの持ち込み防止、逆方向の反例)。

**段 2 (恒久策、段 1 の後で別の着地)**
5. データ欄を**固定オフセット**へ移す: 関数表の容量を予約 (例: 512 スロット = 0x808 まで) し、`sbrk_heap_limit` / `shm_base` をその後ろに固定する。以後の関数追加でデータ欄は動かない。ヘッダ v3 の `kapi_data_off` はその固定値になり、検査は残す。
6. 段 2 は KAPI 構造体の大きさが変わる ABI 変更なので、段 1 の検出が入った後に行う (旧バイナリは段 1 の検査で確実に断られる)。

受入: ホスト試験 (mkos32x がヘッダ v3 を焼く、exec の判定関数: v2 → 断る、v3 で値違い → 断る、一致 → 通す)、NP21/W で旧バイナリ (v62 でビルドしたもの) が `rebuild required` で断られ、作り直したものは動く、shlib の検査、kselftest。
