# KernelAPI v38 仕様書

外部プログラム (OS32X) がカーネル機能を利用するためのAPIテーブル仕様。

---

## §1 概要

| 項目 | 値 |
|------|------|
| バイナリ形式 | OS32X (40バイトヘッダ + フラットバイナリ) |
| ヘッダマジック | 0x4F533332 ('OS32') |
| KAPIテーブルアドレス | 動的算出 (KHEAP_BASE + KHEAP_SIZE) |
| KAPIマジック | 0x4B415049 ('KAPI') |
| プログラムロード先 | 0x400000 |
| 最大プログラムサイズ | 1MB |
| プログラム専用ヒープ | 動的配置 (sbrk_heap_limit, exec_heap 管理下) |
| プログラム専用スタック | 動的配置 (メモリ終端付近、下向き展開) |
| 現在のバージョン | **38** |
| 合計エントリ数 | **165** (ヘッダ2 + 関数ポインタ162 + データフィールド1) |

---

## §2 呼び出し規約

| 対象 | コンパイルフラグ | 規約 |
|------|--------|------|
| カーネル本体 | `gcc -m32` | System V i386 ABI (スタック渡し) |
| KernelAPIラッパー | `__attribute__((cdecl))` または通常 | cdecl/System V |
| 外部プログラム | `gcc -m32 -ffreestanding` | System V i386 ABI |

外部プログラムの `main` は `void main(int argc, char **argv, KernelAPI *api)` のシグネチャを持ちます。crt0 が argc/argv と共に api ポインタを渡します。

---

## §3 ビルド手順

```bash
# 一括ビルド (Makefile利用)
make programs
```
外部プログラムは `programs/` 以下に `.c` を置き、`make programs` を実行することで、`crt0.asm` や `libos32` (newlib-nanoラッパー) とともにリンクされ、`mkos32x.py` によってヘッダが付与された `.bin` が生成されます。

---

## §4 KernelAPI 構造体レイアウト

### ヘッダ

| Offset | フィールド | 説明 |
|--------|-----------|------|
| 0x00 | magic | 0x4B415049 ("KAPI") |
| 0x04 | version | APIバージョン (現在: 38) |

### API関数テーブル

→ **[KAPI_TABLE.md](KAPI_TABLE.md)** — 全162関数のオフセット・プロトタイプ一覧

### API補足ノート

→ **[KAPI_NOTES.md](KAPI_NOTES.md)** — 機能グループ別の詳細説明

---

## §5 変更履歴

| Version | 変更内容 |
|---------|----------|
| 38 | `sys_v86_boot_image` 追加、`loop_attach`/`loop_detach`/`loop_status` 追加、`dev_blk_read`/`dev_blk_write` 追加 |
| 37 | IDE KAPI 削除 (`ide_read_sector`/`ide_write_sector`/`ide_write_sectors`)、`ide_get_info` 追加、`dev_blk_write` 追加 |
| 36 | `sys_v86_boot_native` に `cmdline` 引数追加 (ABI破壊)、`sys_v86_set_debug` 追加 |
| 35 | `sys_get_build_info` 追加 |
| 34 | `sys_v86_boot_freedos`/`sys_v86_boot_physical`/`sys_v86_boot_physical_ex`/`sys_v86_boot_native` 追加 |
| 33 | `kcg_load_font` 追加 |
| 31 | DB API 完成 (`db_open`〜`db_mem_used` 10関数) |
| 29 | DB API 初期導入 |
| 28 | マウスAPI、TVRAM読取・反転、マウスカーソル制御 |
| 27 | `kbd_is_pressed`、FM/SSG個別チャンネル制御 |
| 26 | `paging_is_present` |
| 25 | FDリダイレクト・パイプAPI |
| 24 | `gfx_present_raster` (ラスタパレット) |
| 22 | グラフィックス描画プリミティブ廃止 → libos32gfx へ移行 |

---

*Last Updated: 2026-05-11*
