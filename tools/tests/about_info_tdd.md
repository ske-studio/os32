# About (`about.bin`) の行の組み立て — ホスト TDD 記録

対象: `userland/rust/about/src/info.rs` (と `lib.rs` の `BootImageInfo` の写し) / 試験 `tools/tests/test_about_info.py`
実行: `python3 -B tools/tests/test_about_info.py` (`make check-tools-host` の 1 行)
土台: `info.rs` を `#[path]` でそのまま取り込み、ホストの rustc でビルドして
数値 → 表示の文字列を突き合わせる。KAPI・GUI・エミュレータには触らない。
ROADMAP v1.4 の About dialog。専用の票は無い (2026-09-29 時点)。

## 見ているもの

| 試験 | 中身 |
|---|---|
| `test_sys_version_from_config_h` | `include/config.h` の `SYS_VERSION` をコンパイル時に切り出した値 = Python の正規表現で読んだ値 |
| `test_sys_version_edges` | `SYS_VERSION_X` を拾わない、コメントの中を拾わない、空・無し → `?`、タブ区切り |
| `test_lines` | 各行 (版・Build・Commit・Image CRC・KernelAPI・Memory・Kernel heap・Display の 3 バックエンドと色数・Boot・CPU の 386 / 486 / ベンダ名 / ブランド文字列 (先頭の空白を落とす) / 空白だけのブランド) |
| `test_truncate_and_numbers` | `LINE_MAX` で打ち切る、`hex8` / `dec` の端 |
| `test_family_model` | CPUID leaf 1 の拡張ファミリ・拡張モデル |
| `test_line_fits_window` | `LINE_MAX` ≤ (窓の幅の上限 − 枠 × 2 − column の余白 × 2) / 8、かつ最長の CPU 行 (5 + 47) が入る |
| `test_constants_match_header` / `test_boot_image_info_mirror` / `test_kapi_gate_matches_header` | `GFX_FMT_PACKED8` / `GFX_CAP_HW_*` / `BootImageInfo` の並び / v65 の判定が `os32_kapi_shared.h` と一致 |
| `test_deployed` | [V2] `deploy.yaml` に載っている |

## RED → GREEN

- **2026-09-29 RED**: `test_line_fits_window` — `LINE_MAX = 56` がコメントの「440px = 55 桁」を超えていた。
  窓の矩形は外形 (gshell `wm.rs` の `BORDER_W = 2`) で、column の余白 10px も引くと 416px = 52 桁。
  **GREEN**: `LINE_MAX = 52` (最長の "CPU: " + 47 桁とちょうど同じ)。
- 変異 (実物は書き換えず、`info.rs` の写しを試験に渡して確かめた。5/5 が RED):
  Cirrus の判定を `HW_BLT` だけにする / ブランド文字列の先頭の空白を落とさない /
  family 6 の拡張モデルを足さない / `sys_version` が行頭以外 (コメントの中) も拾う /
  `crc_line` が `ok` を見ない。
