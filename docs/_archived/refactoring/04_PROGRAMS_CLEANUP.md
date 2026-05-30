# programs/ 配下の構造整理

策定日: 2026-05-23  
完了日: 2026-05-24  
ステータス: **完了**

---

## 現状の問題

### 1. programs/ 直下のビルド残骸 (~100個)

`programs/` 直下に `.bin` / `.o` / `.elf` / `.raw` ファイルが約100個散乱。
`.gitignore` で無視されるため Git 上は問題ないが、作業環境が散らかる。

**正規の共有ファイル** (3個、残すべき):
- `crt0.asm` — 全プログラムのスタートアップ (ASM)
- `crt0_c.c` — 全プログラムのスタートアップ (C)
- `os32api.h` — KernelAPI 外部プログラム用ヘッダ

**古い残骸の例**:
- `programs/grep.bin` (14,952B) vs `programs/cmds/grep.bin` (15,016B) — サイズ不一致
- `programs/edit.bin` (32,660B) vs `programs/apps/edit.bin` (33,684B) — サイズ不一致

**shell の正規出力先**:
- `programs/shell.bin` / `shell.elf` / `shell.raw` — programs.mk の正規出力先。変更するなら programs.mk の修正が必要

### 2. ui_demo/ がカテゴリ外

`programs/ui_demo/` は microUI デモ (`ui_demo.c`)。
`programs/apps/` に移動するのが自然。

**programs.mk での定義** (L192-199):
- 専用のビルドルールあり (`$(GFX_OBJ)` + `$(LIBUI_OBJ)` をリンク)
- `programs` 集約ターゲット (L324) に**含まれていない** (意図的？)

### 3. bench/ の位置

`programs/bench/` には `main.o` のみ (ソースなし、ビルド成果物のみ)。
`programs/tests/bench/` にソース (`*.c`) が存在し、programs.mk (L76-86) で
ビルド定義済み。`programs/bench/` は残骸の可能性が高い。

## 変更方針

### Phase 1: 残骸削除 (ビルド変更なし) ✅ 完了 (2026-05-23)

```bash
# programs/ 直下の古いビルド残骸を削除
# 正規ファイル (crt0.asm, crt0_c.c, os32api.h) と
# shell の正規出力 (shell.bin, shell.elf, shell.raw) は残す
cd programs/
# 古い .bin を削除 (shell.bin 以外)
find . -maxdepth 1 -name "*.bin" ! -name "shell.bin" -delete
# 古い .o を削除
find . -maxdepth 1 -name "*.o" -delete
# 古い .elf を削除 (shell.elf 以外)
find . -maxdepth 1 -name "*.elf" ! -name "shell.elf" -delete
# 古い .raw を削除 (shell.raw 以外)
find . -maxdepth 1 -name "*.raw" ! -name "shell.raw" -delete
```

### Phase 2: bench/ 残骸削除 (ビルド変更なし) ✅ 完了 (2026-05-23)

```bash
rm -rf programs/bench/  # main.o のみ、ソースなし
```

### Phase 3: ui_demo/ → apps/ 移動 (programs.mk 変更) ✅ 完了 (2026-05-24)

```bash
mv programs/ui_demo/ programs/apps/ui_demo/
```

**programs.mk の変更**:
```makefile
# 現状 (L192-199)
programs/ui_demo/ui_demo.o: programs/ui_demo/ui_demo.c
programs/ui_demo/ui_demo.elf: ...

# 変更後
programs/apps/ui_demo/ui_demo.o: programs/apps/ui_demo/ui_demo.c
programs/apps/ui_demo/ui_demo.elf: ...
```

## 関連: .gitignore / ルート直下の整理

以下はビルド変更を伴わない安全な整理（本ドキュメントのスコープ外、即実行可能）:

| 対象 | 対応 |
|------|------|
| `.gitignore` に `os32_serial_log.txt` 追加 | 漏れ修正 |
| `.gitignore` に `kernel_objdump.txt` 追加 | 漏れ修正 |
| `tasks/vzeditor_status.md` → `docs/tasks/` 移動 | .gitignore対象なのでGit操作不要 |
| ルートの一時Pythonスクリプト | .gitignore対象で害なし。据え置きまたは削除 |

## リスク

- Phase 1-2: リスクなし（ビルド成果物の削除のみ、`make clean && make all` で再生成）
- Phase 3: programs.mk の 8行変更。`ui_demo` は `programs` ターゲットに含まれておらず影響小
