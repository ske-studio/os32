# Phase D: コマンド改名 & DOS モード廃止

## 目的

1. コマンド名を `vdos` → `v86` に変更
2. **DOS モードを完全廃止** — `v86_boot_freedos` の撤去
3. デフォルト動作をネイティブ PC-98 モードに一本化
4. Auto-Typer は独立オプションとして残す (ネイティブモードでも有用)

---

## D-1. ファイルリネーム

| 旧パス | 新パス |
|--------|--------|
| `programs/cmds/vdos.c` | `programs/cmds/v86.c` |

### Makefile への影響

`build/programs.mk` はワイルドカードベース:

```makefile
C_CMDS = $(wildcard programs/cmds/*.c)
```

リネームだけで自動的にビルド対象が切り替わる。
追加の Makefile 変更は不要。

### app.conf の変更

```diff
-cmds/vdos            32 262144
+cmds/v86             38 262144
```

---

## D-2. DOS モード廃止 — 変更方針

### 廃止する機能

| 機能 | 現状 | 廃止後 |
|------|------|--------|
| `v86_boot_freedos()` | FreeDOS専用ブートパス | **削除** |
| `-dos` フラグ (vdos.c) | DOS モード明示指定 | **削除** |
| KAPI `sys_v86_boot_freedos` | 外部プログラムから呼出可能 | **廃止 → Phase F で撤去** |
| `vdosquit.asm` (COM形式) | DOS INT 0x21 経由で終了 | **アーカイブ** |
| FreeDOS タイムアウト (60秒) | freedos パスでのみ有効 | **削除** (native は無制限) |

### 存続する機能

| 機能 | 理由 |
|------|------|
| `v86_boot_native()` | 唯一のイメージブートパス |
| `v86_boot_physical_fdd()` / `_ex()` | 実FDDブート (ゲスト OS は問わない) |
| Auto-Typer | ネイティブモードでも入力自動化に有用 |
| VSYNC 仮想化 | ネイティブモード標準機能 |
| 画面表示初期化 (DISP ENABLE 等) | ネイティブモード標準機能 |

### Auto-Typer の扱い

Auto-Typer は FreeDOS の `AUTOEXEC.BAT` 入力用に実装されたが、
ネイティブ PC-98 ソフトでもキー入力の自動化に有用 (例: ゲームの起動後の自動操作)。
**`-c` オプション** として独立させ、ネイティブモードでも使えるようにする。

---

## D-3. v86.c 仕様

### Usage

```
Usage: v86 [options] <image_path>
       v86 -fdd [-2dd|-2dd-9]   (boot from physical FDD)

Options:
  image_path  Disk image file (.d88, .fdi, .hdi, .img)
  -fdd        Boot from physical FDD instead of image
  -2dd        Physical FDD as 2DD 640KB (requires -fdd)
  -2dd-9      Physical FDD as 2DD 720KB (requires -fdd)
  -c "cmd"    Auto-type command after boot
  -d          Enable debug logging
  -h          Show help
Exit:         Ctrl+GRPH+DEL
```

### デフォルト動作

- **引数なし** → エラー (`v86 -h` への誘導メッセージ)
- **イメージパスのみ** → ネイティブモード (唯一のモード)
- **`-fdd`** → 実FDD ブート (ネイティブモード)
- **`-c "cmd"`** → Auto-Typer (ネイティブモードで動作)

### コード

```c
int main(int argc, char **argv)
{
    const char *img_path = NULL;
    const char *auto_cmd = NULL;
    int use_fdd = 0;
    int use_2dd = 0;
    int debug_mode = 0;
    int rc, i;

    extern KernelAPI *kapi;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: v86 [options] <image_path>\n");
            printf("       v86 -fdd [-2dd|-2dd-9]\n");
            printf("Options:\n");
            printf("  -fdd      Boot from physical FDD\n");
            printf("  -2dd      Physical FDD as 2DD 640KB\n");
            printf("  -2dd-9    Physical FDD as 2DD 720KB\n");
            printf("  -c \"cmd\"  Auto-type command after boot\n");
            printf("  -d        Enable debug logging\n");
            printf("  -h        Show this help\n");
            printf("Exit: Ctrl+GRPH+DEL\n");
            return 0;
        }
        if (strcmp(argv[i], "-fdd") == 0)   { use_fdd = 1; continue; }
        if (strcmp(argv[i], "-d") == 0)     { debug_mode = 1; continue; }
        if (strcmp(argv[i], "-2dd") == 0)   { use_2dd = 1; continue; }
        if (strcmp(argv[i], "-2dd-9") == 0) { use_2dd = 2; continue; }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            auto_cmd = argv[++i]; continue;
        }
        if (img_path == NULL) {
            img_path = argv[i];
        }
    }

    /* 引数なし + FDDなし → エラー */
    if (img_path == NULL && !use_fdd) {
        printf("Error: image path required. Use 'v86 -h' for help.\n");
        return 1;
    }

    if (debug_mode) {
        kapi->sys_v86_set_debug(1);
    }

    if (use_fdd) {
        /* 実FDDブート */
        if (use_2dd) {
            rc = kapi->sys_v86_boot_physical_ex(0, use_2dd, auto_cmd);
        } else {
            rc = kapi->sys_v86_boot_physical(0, auto_cmd);
        }
    } else {
        /* ネイティブモード (唯一のモード) */
        printf("Booting: %s\n", img_path);
        if (auto_cmd) {
            printf("Auto-type: %s\n", auto_cmd);
        }
        printf("Exit: Ctrl+GRPH+DEL\n");
        rc = kapi->sys_v86_boot_native(img_path);
    }

    if (debug_mode) {
        kapi->sys_v86_set_debug(0);
    }

    if (rc < 0) {
        printf("v86 failed (rc=%d)\n", rc);
        return 1;
    }
    return 0;
}
```

> **注意**: `sys_v86_boot_native` は現在 Auto-Typer に非対応。
> Phase C の `v86_boot_image()` 統合時に、native パスにも `cmdline` 引数を追加する
> (Phase C-3 の `sys_v86_boot_image(path, cmdline, native)` で対応)。

---

## D-4. vdosquit.asm のアーカイブ

`vdosquit.asm` は **DOS INT 0x21 (AH=0x4C)** を使用しているため、
DOS モード廃止に伴いアーカイブ対象とする。

```asm
; vdosquit.asm — DOS依存 (INT 0x21)
mov ah, 0x4C    ← DOS API: プログラム終了
int 0x21        ← DOS API 呼び出し
```

### 移動先

```
programs/system/vdosquit.asm → tools/_archive/vdosquit.asm
```

### V86 脱出機構の代替

`port 0xFE OUT` による `#GP` ハンドラ経由の脱出機構自体は有用だが、
INT 0x21 に依存しない形で再実装する場合は:

```asm
; v86quit.asm — DOS非依存版
org 0x100
    mov al, 0x01
    out 0xFE, al
    hlt             ; DOS API を使わずに停止
```

ただし、ネイティブモードでは **Ctrl+GRPH+DEL** で脱出できるため、
`v86quit` バイナリの必要性は低い。
当面は `vdosquit.asm` をアーカイブするのみとし、代替は作成しない。

---

## D-5. `v86_boot_native` への Auto-Typer 引数追加

DOS モード廃止により、Auto-Typer はネイティブモードで動作させる必要がある。

### カーネル側変更

```c
/* 旧 */
int v86_boot_native(const char *path);

/* 新 (cmdline 引数追加) */
int v86_boot_native(const char *path, const char *cmdline);
```

### セッション初期化への反映

```c
int v86_boot_native(const char *path, const char *cmdline)
{
    /* ... */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;  /* NULL なら Auto-Typer 無効 */
    current_session.auto_delay_remaining = V86_AUTO_TYPE_DELAY;
    /* ... */
}
```

### KAPI 変更

```json
{
    "name": "sys_v86_boot_native",
    "ret": "int",
    "args": ["const char *path", "const char *cmdline"],
    "target": "v86_boot_native"
}
```

> **注意**: `sys_v86_boot_native` のシグネチャ変更は **ABI 破壊** を伴う。
> KAPI_VERSION バンプ (Phase C-3) と同時に行い、
> `make clean` → `make all` を必須とする。

### externs 変更

```diff
-"extern int v86_boot_native(const char *path);"
+"extern int v86_boot_native(const char *path, const char *cmdline);"
```

---

## D 全体の検証

| 項目 | 手順 | 期待結果 |
|------|------|----------|
| 新コマンド | `v86 Ys.D88` | native で起動 |
| Auto-Typer | `v86 -c "test" Ys.D88` | キー入力自動化 |
| 実FDD | `v86 -fdd` | 実FDD ブート |
| 引数なし | `v86` | エラーメッセージ + ヘルプ誘導 |
| 旧コマンド | `vdos` | コマンド未発見 |
| ビルド | `make all` | エラー 0 |
