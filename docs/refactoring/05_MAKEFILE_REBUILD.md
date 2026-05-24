# Makefile 再構築計画

策定日: 2026-05-24  
ステータス: Phase 0 完了

---

## 設計方針

ユーザー判断事項:

| 項目 | 決定 |
|------|------|
| fep_test / fep_engine | **復活** — ビルドに再登録 |
| wildcard vs ハードコード | **wildcard 廃止** — ハードコード維持 |
| boot.mk | **独立維持** — kernel.mk に統合しない |

## 新構造 (目標)

```
Makefile                — トップレベル (全体制御: kernel → libs → programs)
build/
├── config.mk           — 共通設定 (ツールチェーン、フラグ)
├── kernel.mk           — カーネル + SQLite
├── boot.mk             — ブートローダー (独立維持)
├── libs.mk             — ユーザー空間ライブラリ 21個
├── programs.mk         — 外部プログラム (apps/ cmds/ shell/ system/ tests/)
├── deploy.mk           — デプロイ
└── image.mk            — ディスクイメージ・パッケージ
```

## 実行フェーズ

| Phase | 内容 | ステータス |
|-------|------|-----------|
| **Phase 0** | 乖離修正 (残骸削除・欠落修正) | ✅ 完了 |
| **Phase 1** | config.mk インクルードパス整理 | 未着手 |
| **Phase 2** | libs.mk テンプレート統一 | 未着手 |
| **Phase 3** | programs.mk 整理 | 未着手 |
| **Phase 4** | kernel.mk ソースリスト整理 | 未着手 |

---

## Phase 0: 乖離修正 ✅ 完了 (2026-05-24)

### 修正内容

#### 1. programs ターゲットの欠落修正

`make programs` で以下が漏れていた:

| ターゲット | 状況 |
|-----------|------|
| `ui_demo` | 個別定義あり → `programs` に追加 |
| `lz4_cmd` | 個別定義あり → `programs` に追加 |
| `fep_test` | ビルド定義なし → 新規追加 |

#### 2. fep_test / fep_engine 復活

- `lib/fep_engine.c` → `lib/fep_engine_prog.o` としてプログラム用にコンパイル
- `programs/tests/fep_test.c` → `DEFINE_TEST` ではなく専用ルールで登録（`-Ilib` フラグが必要なため）
- `fep_test` を `programs` ターゲットと `.PHONY` に追加

#### 3. 残骸ルール削除

- `programs/libos32gfx/ui.o` のビルドルール (L201-203) — `ui.c` が存在しない

#### 4. filter-out 空振り修正

- `C_APPS` の `filter-out programs/apps/edit.c` — `edit.c` は存在しない (edit/ ディレクトリ版のみ)

#### 5. clean-programs 更新

- `lib/fep_engine_prog.o` の削除を追加
- `programs/apps/ui_demo/` のクリーンを追加

#### 6. ゴミファイル削除

- `programs/cmds/vdos.c.bak` 削除
- `lib/lzss.o`, `lib/lzss_prog.o`, `lib/lz4_decode.o` 削除 (ソースなしの残骸)

---

## Phase 1-4: 今後の計画

### Phase 1: config.mk インクルードパス整理

PROGRAM_FLAGS の `-I` リストが手動列挙で新ライブラリ追加時に漏れやすい。
ライブラリディレクトリを一覧化して自動生成する仕組みを検討。

### Phase 2: libs.mk テンプレート統一

`DEFINE_LIB` テンプレートと手動定義の混在を整理。
特殊要件 (ASM, 分割OBJ, ファイル別フラグ) を扱える拡張テンプレートを検討。

### Phase 3: programs.mk 整理

356行と肥大化。テストプログラムの `filter-out` リストが長大。
カテゴリ別の整理を検討。

### Phase 4: kernel.mk ソースリスト整理

ソースのハードコード維持だが、カテゴリ別変数に分離して可読性を向上。
```makefile
C_KERNEL_CORE = kernel/kernel.c kernel/gdt.c ...
C_V86         = kernel/v86.c kernel/v86_mem.c ...
C_DRIVERS     = drivers/kbd.c drivers/serial.c ...
C_FS          = fs/vfs.c fs/ext2_super.c ...
C_KERNEL      = $(C_KERNEL_CORE) $(C_V86) $(C_DRIVERS) $(C_FS) ...
```
