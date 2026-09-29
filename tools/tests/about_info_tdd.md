# About (`about.bin`) の中身 = `ver` の出力 — ホスト TDD 記録

対象: `userland/rust/about/src/info.rs` (と `lib.rs` の `BootImageInfo` の写し・窓の寸法)、
`userland/shell/cmd_base.c` の `cmd_ver` / 試験 `tools/tests/test_about_info.py` +
ハーネス `tools/tests/ver_about_host.c`
実行: `python3 -B tools/tests/test_about_info.py [--mutate]` (`make check-tools-host` の 1 行)
ROADMAP v1.4 の About dialog。専用の票は無い。

## 仕様 (ユーザー指示 2026-09-29)

**About の表示内容は `ver` の表示内容と同じ**。以前の About は CPU (CPUID)・Memory・
Kernel heap・Display・Boot の行も出していたが外し、`ver` の行 — 版の行
(`PC-9801 OS32 v2.0 (Ring3 Native)`)、機器の 7 行、`API: vN`、`Build:`、KAPI v65 以上で
`boot_image_info` が 0 を返したときだけ `Commit:` と `Image CRC:` — に揃えた。
違うのは `ver` の 2 行目以降の字下げ ("  ") が無いことだけ。起動の入口は従来どおり
Start → Programs (root メニューへの項目は足していない)。

## 土台 — 食い違いの検出

`ver` はシェル (C)、About は Rust なので文字列の組み立てそのものは共有できない
(版だけは `include/config.h` の `SYS_VERSION` を About がコンパイル時に切り出して共有)。
代わりに**両方の実物を同じ入力で回して突き合わせる**:

- ver 側: `ver_about_host.c` が実物の `cmd_base.c` を 1 行も写さず `#include` し、
  KernelAPI の `kprintf` / `sys_get_build_info` / `boot_image_info` / `version` だけを贋物にして
  `cmd_ver` を回す。
- About 側: `info.rs` を `#[path]` でそのまま取り込み、`ver_lines` + `boot_shown` (v65 の判定) で
  行を出す (`lib.rs` と同じ 32 / 24 バイトの詰め方)。

## 見ているもの

| 試験 | 中身 |
|---|---|
| `test_about_equals_ver` | 11 組の入力 (ローダ HDD / FD / 0 / 7、CRC 無し、空の commit、`boot_image_info` 失敗、KAPI 65 / 64 / 0、桁の最大とバッファ超えの build / commit) で、ver の行の字下げを落としたもの = About の行 |
| `test_about_lines_pinned` | 代表 1 組を文字列で固定 (両方が同じ向きに壊れても通らないように) |
| `test_boot_lines_conditions` | Commit / Image CRC の出る・出ない、ローダ名 `HDD` / `FD` / `?`、`none (loader did not record)` |
| `test_longest_lines_fit` | 最長の行 (Image CRC の 50 桁) が `LINE_MAX` に収まり、行数が `NLINES_MAX` に収まる |
| `test_sys_version_from_config_h` / `test_sys_version_edges` | 版の切り出し (`SYS_VERSION_X` やコメントの中を拾わない、空・無し → `?`) |
| `test_truncate_and_numbers` | `LINE_MAX` で打ち切る、`hex8` / `dec` の端 |
| `test_line_fits_window_width` | `LINE_MAX` ≤ (窓の幅の上限 − 枠 × 2 − column の余白 × 2) / 8 |
| `test_lines_fit_window_height` | 最大 12 行 + 区切り 2 本 + OK の段が窓の高さの上限 (312px) に入り、上限は 640x400 に収まる |
| `test_constants_match_header` / `test_boot_image_info_mirror` / `test_kapi_gate_matches_header` | `BootImageInfo` の並びと source の値、v65 の判定が `os32_kapi_shared.h` と一致、`lib.rs` が判定と組み立てを `info.rs` から使う |
| `test_deployed` | [V2] `deploy.yaml` に載っている |

## RED → GREEN

- **2026-09-29 (初版)**: `test_line_fits_window` — `LINE_MAX = 56` が窓の幅を超えていた → `52`。
- **2026-09-29 (ver に揃える)**: 新しい試験を旧 `info.rs` に当てると RED
  (`ver_lines` / `BootImage` / `boot_shown` が無くコンパイルが通らない = 旧 About は ver と別の行を出す)。
  **GREEN**: `info.rs` を `ver` の行の組み立てに書き換え、`lib.rs` から CPUID の識別と
  Memory / heap / Display / Boot の取得を外した。途中で 1 回 RED: 空の build で ver は
  `"  Build: "` (末尾の空白つき) を出す — 試験側の字下げの落とし方を `strip()` から
  先頭 2 文字だけに直した (About 側は正しかった)。
- 変異 (`--mutate`、写しを環境変数 `OS32_ABOUT_INFO_RS` / `OS32_VER_CMD_BASE_C` で差し込む。
  実物は書き換えない。対照 = 変異なしの写しは GREEN)。14/14 が RED:
  - info.rs: 機器の行の文言 / 1 行目の " (Ring3 Native)" を落とす / `API:` を `KernelAPI:` に /
    ローダ名の取り違え / `none` の文言 / 空の commit を `unknown` に / v65 の判定が戻り値を見ない /
    v64 から出す / CRC 行の長さを落とす / `sys_version` が行頭以外も拾う
  - cmd_base.c (ver だけ変えて About を直し忘れた): 機器の行の文言 / 行を 1 本足す /
    v60 から出す / `none` の文言

## 未確認

- ゲスト (NP21/W・実機) での窓の見た目は未確認 (ホスト試験は寸法の計算まで)。
