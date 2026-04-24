# ext2 DINDバグ — 根本原因解析結果

## 確定した事実 (NHDバイナリ解析による)

### 1. inode.size の不整合
```
inode.size = 372600 (0x5AF78)  ← ext2上の値
expected   = 372632 (0x5AF98)  ← 正しい値 (HostDrv)
差分       = 32 bytes
```

### 2. ブロックマッピングは正常
```
ブロック数 = 734 sectors (= 364 blocks × 2)
IND block  = 49491  (ブロック 12-267)
DIND block = 49500  (ブロック 268-363)
  DIND[0] → IND 49561: 96 entries, last=[95] → file_block=363 ✓
```

全364ブロック (0-363) が正しく割り当てられている。

### 3. 最終ブロック (363) のデータ破損

```
ext2 block 363 bytes 888-919: ALL ZEROS
Host file  bytes 888-919:     AA BD 1D 00 DC BD 1D 00 23 BE 1D 00 10 BE 1D 00 ...
```

- バイト 0-887: 正しく書き込まれている (推定)
- **バイト 888-919: ゼロ (データ未書き込み)**
- バイト 920-1023: ゼロ (ファイル末尾以降、正常)

## デバッグ結果 (2026-04-24)

### Option B: NHDオフライン検証 ✅ 成功
- Linuxのext2ドライバで正しいデータ (372632 bytes) を `/tmp/os32` にコピー
- NHDをデプロイしてNP21/Wで起動
- **OS32のext2読み取り (DIND含む) は正常動作**
- 結論: **読み取りパスに問題なし、書き込みパスにバグあり**

> [!WARNING]
> Option Bの副作用: Linux ext2マウント後、OS32のext2ドライバとの
> 不整合で `FATAL: shell.bin load failed` が発生。
> ext2フォーマット + フルデプロイで復旧した。
> **今後はLinux側でのext2直接マウントは避けること。**

### Option A: kprintfデバッグ ✅ バグ再現せず
- `ext2_write_stream` にDIND領域の詳細ログを追加
- hsync -f sys で sqlite.bin を再書き込み (rm後 / 上書き両方テスト)
- 結果: 全ケースで `rem=0 brk=0 isz=372632` — **正常終了**
- バグは特定のディスク断片化条件でのみ発生する稀な問題と推定

### Option C: 防御的修正 ✅ 適用済み
- `ext2_write_stream` ループ後に `remaining > 0` 検出ログを追加
- `[E2W] INCOMPLETE ino=%d rem=%d/%d off=%d` でエラー時に警告
- 将来的にバグが再発した場合、即座に原因箇所を特定可能

## 現在のステータス

- **ext2 DIND 読み取り**: ✅ 正常 (Option Bで確証)
- **ext2 DIND 書き込み**: ⚠️ バグは再現不可だが防御ログを追加済み
- **sqlite.bin (372632 bytes)**: ✅ 正しいサイズでext2に格納済み
- **SQLite KAPI (db_test.bin)**: ❌ `db_open(:memory:)` 後にハング — 別件
