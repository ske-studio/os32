# ライブラリ命名規則の統一

策定日: 2026-05-23  
ステータス: 未着手

---

## 現状の問題

21ライブラリ中、18個は `libos32*` プレフィックスだが、3個はプレフィックスなし。

| 現在の名前 | 統一後の名前 | 用途 | テンプレート |
|-----------|------------|------|------------|
| `libfiler` | `libos32filer` | GFXファイルブラウザ | 手動定義 (OBJ2グループ分割) |
| `libmd` | `libos32md` | Markdownパーサー+レンダラー | 手動定義 (ファイル別フラグ) |
| `libtilemap` | `libos32tilemap` | タイルマップ合成エンジン | 手動定義 (ASMソースあり) |

> **注意**: 3ライブラリとも `DEFINE_LIB` テンプレートを使用していない特殊なビルド定義。

## 変更対象

### 1. ディレクトリリネーム

```
programs/libfiler/    → programs/libos32filer/
programs/libmd/       → programs/libos32md/
programs/libtilemap/  → programs/libos32tilemap/
```

### 2. ヘッダリネーム

```
libfiler.h    → libos32filer.h
libmd.h       → libos32md.h
libtilemap.h  → libos32tilemap.h
```

### 3. build/libs.mk の変更

#### libtilemap (L131-145)

```makefile
# 現状
TILEMAP_SRC = $(wildcard programs/libtilemap/*.c)
TILEMAP_ASM_SRC = $(wildcard programs/libtilemap/*.asm)

# 変更後
TILEMAP_SRC = $(wildcard programs/libos32tilemap/*.c)
TILEMAP_ASM_SRC = $(wildcard programs/libos32tilemap/*.asm)
```

#### libmd (L153-161)

```makefile
# 現状
programs/libmd/md_parse.o: ...
programs/libmd/md_render.o: ... -Iprograms/libfiler ...

# 変更後
programs/libos32md/md_parse.o: ...
programs/libos32md/md_render.o: ... -Iprograms/libos32filer ...
```

#### libfiler (L163-172)

```makefile
# 現状
programs/libfiler/filer_core.o: ...
FILER_OBJ = programs/libfiler/filer_core.o
FILER_DRAW_OBJ = programs/libfiler/filer_draw.o

# 変更後
programs/libos32filer/filer_core.o: ...
FILER_OBJ = programs/libos32filer/filer_core.o
FILER_DRAW_OBJ = programs/libos32filer/filer_draw.o
```

### 4. プログラムの #include 変更

`#include "libfiler.h"` 等を使用している全プログラムを検索して更新:

```bash
# 影響範囲の確認コマンド
grep -r '#include.*libfiler\.h\|#include.*libmd\.h\|#include.*libtilemap\.h' programs/
```

### 5. programs.mk のリンク依存

各プログラムの ELF ターゲットで `$(FILER_OBJ)`, `$(TILEMAP_OBJ)`, `$(MDLIB_OBJ)` を
参照している箇所。変数名はそのまま維持可能（変数の中身がパス変更されるだけ）。

## リスク

- **影響範囲**: libs.mk + programs.mk + 全プログラムの #include
- **コンパイルエラー**: インクルードパス不一致で即座に発覚するため、サイレント破壊のリスクは低い
- **Git履歴**: ディレクトリリネームにより git log --follow が必要になる

## 判断

機能的な問題はないため、**コスト対効果を考慮して優先度は低い**。
他のリファクタリングと同時実施することでテストコストを共有するのが望ましい。
