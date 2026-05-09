# Phase 3: KAPI + シェルコマンド更新

## 目的

- KAPI `dev_blk_read` の内部実装を `dev_blk_read_lba` に切り替え
- KAPI シグネチャは変更しない (外部プログラム互換維持)
- dd コマンドの内部呼び出しを更新

## 前提条件

- Phase 2 完了 (Device API 2系統化済み)

---

## 変更ファイル一覧

### 1. kapi_generated.c — ラッパー実装変更

**変更前:**
```c
int __cdecl wrap_dev_blk_read(const char *dev_name, u32 lba,
                               int count, void *buf)
{
    Device *d = dev_find(dev_name);
    if (!d || !d->blk_read) return -1;
    return d->blk_read(d, (int)lba, count, buf);
}
```

**変更後:**
```c
int __cdecl wrap_dev_blk_read(const char *dev_name, u32 lba,
                               int count, void *buf)
{
    Device *d = dev_find(dev_name);
    if (!d) return -1;
    return dev_blk_read_lba(d, lba, count, buf);
}
```

> **注意**: `kapi_generated.c` は `tools/kapi.json` から自動生成される。
> `wrap_dev_blk_read` のカスタム実装部分 (`kapi/kapi_dev.c` 等) を確認し、
> 正しいファイルを編集すること。

### 2. kapi.json — 変更なし

KAPI シグネチャ `dev_blk_read(const char*, u32, int, void*)` は変更しない。
外部プログラムからは LBA で指定する API のまま。

### 3. cmd_mnt.c — dd コマンド内部変更

**変更前:**
```c
if (g_api->dev_blk_read(dev_name, (u32)(lba + i), 1, buf) != 0) {
```

**変更後:**
変更なし — `g_api->dev_blk_read` は KAPI 経由で `dev_blk_read_lba` を呼ぶため、
外部プログラム側のコード変更は不要。

---

## テスト計画

| # | テスト | 期待結果 |
|---|---|---|
| 1 | `dd lo0 lba=0 count=32` (D88) | LBA=16 エラー停止 |
| 2 | `dd lo0 count=1232 file=/host/fdi.bin` (FDI) | ground truth 一致 |
| 3 | `dd hd0 lba=0 count=1` (IDE) | ブートセクタ読み出し |

## コミット戦略

```
refactor: kapi: use dev_blk_read_lba in wrap_dev_blk_read
```
