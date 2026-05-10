# Phase 0: 前提条件の整備

## 目的

`kapi.json` の version フィールドと `KAPI_VERSION` マクロの不整合を解消する。

## 背景

| ファイル | 現在値 | 正値 |
|---------|--------|------|
| `tools/kapi.json` → `"version"` | 35 | **37** |
| `include/os32_kapi_shared.h` → `KAPI_VERSION` | 37 | 37 (変更なし) |

`kapi.json` の version フィールドは `mkos32x.py` が `app.conf` の
`min_api_ver` と比較する用途で使われている。
`KAPI_VERSION` マクロとは独立した値だが、同期が取れていないと混乱の原因になる。

## 作業

### 0-1. kapi.json version を 37 に更新

```diff
 {
-  "version": 35,
+  "version": 37,
   "data_fields": [
```

**対象ファイル**: `tools/kapi.json` L2

### 0-2. 確認

```bash
make all
```

ビルドエラーがないこと、既存プログラムが正常動作することを確認。

## 注意

- **`KAPI_VERSION` は Phase C-3 まで変更しない** (37 のまま)。
- Phase C-3 で新 API (`sys_v86_boot_image`) 追加時に 37 → 38 にバンプする。
