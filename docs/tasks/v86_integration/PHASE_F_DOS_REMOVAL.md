# Phase F: DOS モード完全撤去 (カーネル側)

## 目的

Phase D でユーザー空間 (v86.c) から DOS モードを廃止した後、
カーネル側の `v86_boot_freedos()` と関連 KAPI を完全に撤去する。

> **Phase D → F の分離理由**:
> Phase D は外部プログラム (v86.c) のみの変更で完結する。
> Phase F はカーネル関数の削除と KAPI 構造体の変更を伴い、
> `make clean` → `make all` が必須。段階的に進めることでリスクを最小化する。

---

## F-1. `v86_boot_freedos` の削除

### 対象ファイル

- `kernel/v86_session.c`
- `kernel/v86_session.h`

### 削除する関数

```c
int v86_boot_freedos(const char *path, const char *cmdline);
```

`v86_session.c` L470-715 の約 **245行** を削除。

### 依存関係の確認

| 呼び出し元 | 対応 |
|------------|------|
| `programs/cmds/vdos.c` | Phase D で v86.c に書き換え済み (呼び出しなし) |
| `kapi/kapi_generated.c` | F-2 で KAPI 削除 |
| `include/os32_kapi_generated.h` | F-2 で自動再生成 |

### v86_boot_native との統合

Phase C で `v86_boot_image()` に統合済みの場合は、
`v86_boot_freedos` は既にラッパーとして最小化されているため、
ラッパーごと削除するだけで完了。

Phase C 未実施の場合は、`v86_boot_freedos` のイメージ判定ロジックは
`v86_boot_native` と重複しているため、単純削除で問題ない。

---

## F-2. KAPI から `sys_v86_boot_freedos` を削除

### 対象ファイル

- `tools/kapi.json`
- `include/os32_kapi_shared.h` (KAPI_VERSION バンプ)

### kapi.json の変更

```diff
-    {
-      "name": "sys_v86_boot_freedos",
-      "ret": "int",
-      "args": ["const char *path", "const char *cmdline"],
-      "target": "v86_boot_freedos"
-    },
```

### externs の変更

```diff
-"extern int v86_boot_freedos(const char *path, const char *cmdline);"
```

### KAPI_VERSION バンプ

Phase C-3 と同時に行う場合:

```diff
-#define KAPI_VERSION      37
+#define KAPI_VERSION      38   /* sys_v86_boot_freedos 削除, sys_v86_boot_image 追加 */
```

> **注意**: KAPI 構造体から関数ポインタを削除すると、
> **関数テーブルのオフセットがずれて ABI が壊れる**。
> `sys_v86_boot_freedos` のスロットを NULL で埋めるか、
> 完全に削除するかの選択が必要。
>
> **推奨**: 完全削除 + KAPI_VERSION バンプ。
> 旧バイナリは `min_api_ver` チェックで起動を拒否される。

### ビルド手順

```bash
make clean
make all
```

---

## F-3. 実FDD ブート関数の整理

`v86_boot_physical_fdd()` / `v86_boot_physical_fdd_ex()` は
「物理FDDから任意のソフトをブート」する汎用機能であり、
DOS 固有ではない。**存続させる**。

ただし、以下の軽微な修正を行う:

### デバッグヘッダの変更

```diff
-    v86_debug_write_header("FreeDOS", path, cmdline);
+    v86_debug_write_header("Native", "(physical)", cmdline);
```

### ネイティブモード設定の追加

現在の `v86_boot_physical_fdd` はネイティブモード設定
(`v86_native_mode = 1`, `v86_timeout_ticks = 0`) を行っていない。
DOS モード廃止に伴い、全ブートパスをネイティブモードにする:

```c
int v86_boot_physical_fdd(int drv, const char *cmdline)
{
    /* ... セッション初期化 ... */
    v86_timeout_ticks = 0;  /* タイムアウト無効 */
    v86_native_mode = 1;    /* ネイティブモード */
    /* ... 以降の処理 ... */
}
```

---

## F-4. FreeDOS 関連資材のアーカイブ

Phase E と合わせて実施:

| 移動元 | 移動先 |
|--------|--------|
| `tools/fdkernel/nec98/` | `tools/_archive/fdkernel/nec98/` |
| `programs/system/vdosquit.asm` | `tools/_archive/vdosquit.asm` |

### 残すもの

| ファイル | 理由 |
|---------|------|
| `tools/freedos98/` | FreeDOS FDD イメージとして参考用に残す |

---

## F-5. v86_session.h のクリーンアップ

### 削除するプロトタイプ

```diff
-int  v86_boot_freedos(const char *path, const char *cmdline);
```

### 変更するプロトタイプ

```diff
-int  v86_boot_native(const char *path);
+int  v86_boot_native(const char *path, const char *cmdline);
```

---

## F 全体の検証

| 項目 | 手順 | 期待結果 |
|------|------|----------|
| ビルド | `make clean` → `make all` | エラー 0 |
| D88 回帰 | `v86 Ys.D88` | タイトル画面到達 |
| FDI 回帰 | `v86 game.fdi` | ネイティブブート |
| 実FDD | `v86 -fdd` | ネイティブブート |
| Auto-Typer | `v86 -c "test" Ys.D88` | キー入力自動化 |
| 旧KAPI | 旧 `vdos.bin` (API 32) 実行 | `min_api_ver` チェックで拒否 |
| grep | `grep -r freedos kernel/` | 0 ヒット (コメント除く) |
