# Phase C: セッション起動 API 統合

## 目的

`v86_boot_freedos()` と `v86_boot_native()` で約200行にわたり複製されている
イメージ判定ロジックを共通関数に集約する。

> **DOS モード廃止** (Phase D/F) に伴い、`v86_boot_freedos` は本 Phase で
> `v86_boot_native` に統合した上で、Phase F で完全削除する。

---

## C-1. ヘルパ関数新設

### 対象ファイル

- `kernel/v86_session.c`

### 新規関数

```c
/**
 * v86_open_image_to_loop — イメージファイルを loop_dev にアタッチし、
 *                           IPL 情報を返す。
 *
 * D88/FDI/HDI/RAW 判定は loop_dev_attach_fd に丸投げ。
 * IPL 取得は loop_dev 経由で track0/head0/sect1 を1回読むだけ。
 *
 * 戻り値: 0=成功, -1=オープン失敗, -2=フォーマット不正, -3=IPL読み込み失敗
 *
 * out_slot: アタッチされた loop_dev スロット番号
 * out_ipl_off: IPL データのファイルオフセット (D88: trk0+16, FDI: hdr_size, RAW: 0)
 * out_ipl_size: IPL セクタサイズ (BPS)
 */
static int v86_open_image_to_loop(const char *path, int *out_slot,
                                   u32 *out_ipl_off, u32 *out_ipl_size);
```

### 実装概要

```c
static int v86_open_image_to_loop(const char *path, int *out_slot,
                                   u32 *out_ipl_off, u32 *out_ipl_size)
{
    int fd, slot, ret;
    u8 ipl_buf[1024];

    fd = vfs_open(path, 0);
    if (fd < 0) return -1;

    /* 空きスロットを探してアタッチ */
    for (slot = 0; slot < 4; slot++) {
        ret = loop_dev_attach_fd(fd, slot);
        if (ret == 0) break;
    }
    if (ret != 0) {
        vfs_close(fd);
        return -2;
    }

    /* v86_disk にアタッチ */
    v86_disk_attach_loop(slot);

    /* IPL: track0/head0/sect1 を読む */
    ret = loop_dev_read_chs(slot, 0, 0, 1, ipl_buf);
    if (ret != 0) {
        loop_dev_detach(slot);
        vfs_close(fd);
        return -3;
    }

    /* D88 の場合: IPL はトラック0の最初のセクタデータ部
     * FDI/HDI/RAW: data_offset + 0 */
    *out_slot = slot;
    {
        u16 bps;
        loop_dev_get_geometry(slot, NULL, NULL, NULL, &bps, NULL);
        *out_ipl_size = (u32)bps;
    }

    return 0;
}
```

> **注意**: `loop_dev_attach_fd` は fd の所有権を呼び出し側に残す
> (`owns_fd = 0`)。fd は `v86_session_run_core` 終了後に
> `v86_disk_clear()` (detach) → `vfs_close(fd)` の順で解放する。

---

## C-2. boot_native への一本化

### 対象ファイル

- `kernel/v86_session.c`

### 方針

DOS モード廃止により、`v86_boot_image` に `native` フラグは不要。
全ブートパスがネイティブモードとなる。

```c
/**
 * v86_boot_image — イメージファイルから V86 セッションを起動 (内部関数)
 *
 * path: イメージファイルパス
 * cmdline: Auto-Typer コマンド (NULL=なし)
 *
 * 常にネイティブモード (タイムアウト無効, VSYNC仮想化, 画面初期化)
 */
static int v86_boot_image(const char *path, const char *cmdline);
```

### 統合する処理 (全て native 動作)

| 項目 | 値 |
|------|----|
| `v86_timeout_ticks` | 0 (無効: Ctrl+GRPH+DEL で脱出) |
| `v86_native_mode` | 1 |
| Auto-Typer | `cmdline` (NULL なら無効) |
| VSYNC 仮想化 | `v86_vsync_init()` / `v86_vsync_cleanup()` |
| 画面表示初期化 | DISP ENABLE + GDC START + 16色モード |
| デバッグヘッダ | `"Native"` |

### 公開関数

```c
/* v86_boot_native: cmdline 引数を追加 (Phase D-5) */
int v86_boot_native(const char *path, const char *cmdline)
{
    return v86_boot_image(path, cmdline);
}

/* v86_boot_freedos: Phase F で削除するまで互換ラッパーとして残す */
int v86_boot_freedos(const char *path, const char *cmdline)
{
    return v86_boot_image(path, cmdline);
}
```

> **注意**: `v86_boot_freedos` の互換ラッパーは Phase F で削除される。
> 旧バイナリが `sys_v86_boot_freedos` を呼んでも native 動作する。

### コード量見積もり

| 現在 | 統合後 |
|------|--------|
| `v86_boot_freedos`: ~240行 | `v86_boot_image`: ~120行 |
| `v86_boot_native`: ~210行 | `v86_boot_native` ラッパー: ~4行 |
| 合計: ~450行 | `v86_boot_freedos` ラッパー: ~4行 (Phase F で削除) |
| | 合計: ~128行 (**-72%**) |

---

## C-3. KAPI 拡張

### 対象ファイル

- `tools/kapi.json`
- `include/os32_kapi_shared.h`

### 新規 KAPI 関数

`kapi.json` の `api` 配列末尾に追加:

```json
{
    "name": "sys_v86_boot_image",
    "ret": "int",
    "args": ["const char *path", "const char *cmdline"],
    "target": "v86_boot_image_kapi"
}
```

> **注意**: `native` 引数は不要 (常にネイティブモード)。
> `v86_boot_image` は `static` 関数のため、
> KAPI ラッパー `v86_boot_image_kapi` を公開関数として新設する。

### externs 追加

```json
"extern int v86_boot_image_kapi(const char *path, const char *cmdline);"
```

### KAPI_VERSION バンプ

```diff
-#define KAPI_VERSION      37
+#define KAPI_VERSION      38   /* sys_v86_boot_image 追加, sys_v86_boot_native 引数変更 */
```

### kapi.json version 同期

```diff
-  "version": 37,
+  "version": 38,
```

### ビルド手順

```bash
make clean
make all
```

> **重要**: KAPI 構造体変更時は `make clean` → `make all` を必ず実行。
> 古い `syscalls.o` が残ると ABI 不整合で `malloc` が全て ENOMEM で失敗する。

---

## C 全体の検証

| 項目 | 手順 | 期待結果 |
|------|------|----------|
| KAPI版数 | `make programs` 後、`KAPI_VERSION` 突合 | 38 |
| native 経路 | `v86 Ys.D88` | タイトル画面到達 |
| Auto-Typer | `v86 -c "test" Ys.D88` | キー入力自動化 |
| 新API | `sys_v86_boot_image("Ys.D88", NULL)` | native 起動 |
| freedos互換 | `sys_v86_boot_freedos("game.fdi", NULL)` | native として起動 |
| ビルド | `make all` | エラー 0 |
