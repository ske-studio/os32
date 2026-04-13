# OS32 技術仕様書

PC-9801シリーズ向け 32ビット ベアメタルOS

---

## ドキュメント一覧

### 本体仕様

| ファイル | 内容 |
|---------|------|
| [01_system.md](01_system.md) | **第1部** システム概要 — アーキテクチャ、ブートシーケンス |
| [02_memory.md](02_memory.md) | **第2部** メモリマップ — 物理メモリ配置、DMA制約 |
| [03_disk.md](03_disk.md) | **第3部** ディスクレイアウト — FDD仕様、セクタ配置 |
| [04_interrupts.md](04_interrupts.md) | **第4部** 割り込みシステム — IDT/PIC/PIT |
| [05_drivers.md](05_drivers.md) | **第5部** デバイスドライバ — KBD/Serial/FM/FDD/GFX |
| [06_filesystem.md](06_filesystem.md) | **第6部** ファイルシステム — VFS/ext2/IDE |
| [07_shell.md](07_shell.md) | **第7部** シェル — コマンド一覧、入力機能 |
| [08_build.md](08_build.md) | **第8部** ビルドシステム — パイプライン、ディレクトリ構造 |
| [09_exec.md](09_exec.md) | **第9部** 外部プログラム実行 — OS32X/exec |
| [10_notes.md](10_notes.md) | **第10部** 既知の制約と注意事項 |

### API・開発ガイド

| ファイル | 内容 |
|---------|------|
| [KAPI_SPEC.md](KAPI_SPEC.md) | KernelAPI v24 仕様書 — 108エントリテーブル (関数107 + データフィールド1) |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 開発ガイドライン — ルール、手順、ロードマップ |

---

*OS32 Technical Specification — Version 8.0*
*Last Updated: 2026-04-08*
