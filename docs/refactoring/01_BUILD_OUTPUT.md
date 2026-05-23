# ビルド成果物の出力先統合

策定日: 2026-05-23  
ステータス: 未着手

---

## 現状の問題

ビルド成果物がプロジェクトルート直下に出力されており、作業ディレクトリが散らかる。

### ルート直下に出力されるファイル (6個)

| ファイル | サイズ | 生成元 | 参照元 |
|---------|--------|--------|--------|
| `kernel.elf` | 676KB | `kernel.mk:107` ターゲット名直書き | `kernel.mk` (kernel.bin, sqlite.bin の依存元) |
| `kernel.bin` | 210KB | `kernel.mk:110` ターゲット名直書き | `kernel.mk` (vmkernel.lz4 の依存元) |
| `kernel.map` | 386KB | `config.mk:57` LDFLAGS 内 `-Map=kernel.map` | なし (デバッグ用) |
| `sqlite.bin` | 373KB | `kernel.mk:119` ターゲット名直書き | `kernel.mk` (vmkernel.lz4 の依存元) |
| `vmkernel.lz4` | 444KB | `kernel.mk:127` ターゲット名直書き | `deploy.mk:7,13`, `image.mk:11`, `Makefile:46` |
| `unicode.bin` | 131KB | `programs.mk:268` 内で生成 | `image.mk:14` |

### 既にサブディレクトリに出力されるもの (問題なし)

- `boot/boot_fat.bin`, `boot/loader_*.bin` → `boot/` 内
- `programs/*.bin`, `programs/cmds/*.bin` 等 → `programs/` サブディレクトリ内
- `images/os32_boot.d88`, `images/os32_boot.img`, `images/os32_install.iso` → `images/` 内

## 変更方針

### 出力先変数の導入

`build/config.mk` に出力ディレクトリ変数を追加:

```makefile
# ビルド成果物出力ディレクトリ
BUILD_OUT = build/out
```

### 変更対象ファイル

| ファイル | 変更内容 |
|---------|---------|
| `build/config.mk` | `BUILD_OUT` 変数追加。`-Map=$(BUILD_OUT)/kernel.map` に変更 |
| `build/kernel.mk` | ターゲット名を `$(BUILD_OUT)/kernel.elf` 等に変更 (6箇所) |
| `build/deploy.mk` | `vmkernel.lz4` → `$(BUILD_OUT)/vmkernel.lz4` (2箇所) |
| `build/image.mk` | `vmkernel.lz4`, `unicode.bin` → `$(BUILD_OUT)/` 接頭辞 (2箇所) |
| `build/programs.mk` | `unicode.bin` → `$(BUILD_OUT)/unicode.bin` (1箇所) |
| `Makefile` | `all` ターゲットの依存リスト更新。`clean` ターゲットに `$(BUILD_OUT)/` 追加 |

### パス参照の完全マップ

```
kernel.mk:107  kernel.elf:  (定義)
kernel.mk:110  kernel.bin: kernel.elf  (定義 + 参照)
kernel.mk:119  sqlite.bin: kernel.elf  (定義 + 参照)
kernel.mk:127  vmkernel.lz4: kernel.bin sqlite.bin  (定義 + 参照×2)
config.mk:57   -Map=kernel.map  (LDFLAGS内)
programs.mk:268  unicode.bin 生成  (定義)
deploy.mk:7   vmkernel.lz4  (参照)
deploy.mk:13  vmkernel.lz4  (参照)
image.mk:11   vmkernel.lz4  (参照)
image.mk:14   unicode.bin  (参照)
Makefile:46    kernel.bin sqlite.bin vmkernel.lz4  (参照×3)
Makefile:50    clean ターゲット  (削除対象)
```

## リスク

- **影響範囲**: Makefile 6ファイル、計15箇所以上のパス変更
- **互換性**: `make clean` で旧パスのファイルも掃除する移行期間が必要
- **CI/スクリプト**: `tools/hostdrv_deploy.py`, `tools/nhd_deploy.py` が直接パスを参照していないか要確認
  - → deploy.mk 経由で呼ばれるため、ツール自体の変更は不要と推定

## 実行タイミング

V86 開発のマイルストーン後、他のビルド変更と同時に実施することを推奨。
