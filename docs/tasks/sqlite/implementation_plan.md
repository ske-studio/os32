# ext2 DIND 書き込みバグ — 外部テストプログラムによる検証計画

## 背景

ext2の DIND (Double Indirect Block) 書き込みで、372KBファイルの末尾32バイトが欠落するバグを調査中。hsync経由では再現しなかったため、外部プログラムとして制御されたテストを実施する。

### これまでの結果
- **Option B (NHDオフライン検証)**: 読み取りパスは正常 ✅
- **Option A (kprintfデバッグ)**: hsync経由では再現せず
- **Option C (防御的修正)**: `[E2W] INCOMPLETE` ログ追加済み

### なぜ外部プログラムか？
1. hsyncはシリアルI/Oのタイムアウト問題があり、デバッグログが欠損する
2. 外部プログラムなら画面に直接出力でき、テスト条件を厳密に制御できる
3. バグ再現条件（断片化、上書き、チャンクサイズ）を変えて繰り返しテスト可能
4. `memcmp` でバイト単位の一致検証ができる

## 提案する変更

### テストプログラム: `e2test.c`

#### [NEW] [e2test.c](file:///mnt/c/WATCOM/src/os32/programs/tests/e2test.c)

KAPI を直接使用する外部テストプログラム。newlib の `open()`/`write()`/`read()` 経由。

**テスト内容:**

| テスト | 内容 | 目的 |
|--------|------|------|
| Test 1 | 280KB ファイル作成→読み戻し→比較 | DIND境界 (268KB) 跨ぎ、新規ファイル |
| Test 2 | 同ファイルを上書き→読み戻し→比較 | 既存ブロック上書きパス |
| Test 3 | 372KB ファイル (元バグサイズ) | 正確な再現条件 |
| Test 4 | 64KB チャンク分割書き込み | hsyncと同じ書き込みパターン |

**テストデータ**: 各バイト = `(offset * 7 + 0x5A) & 0xFF` の擬似乱数パターン。全ゼロや連番では見逃すバグを検出。

**検証方法**:
1. ファイル作成: `open(O_WRONLY | O_CREAT | O_TRUNC)` → `write()` → `close()`
2. 読み戻し: `open(O_RDONLY)` → `read()` → `close()`
3. バイト比較: `memcmp` で全データを検証、不一致箇所をオフセット付きで報告
4. サイズ検証: `stat()` でファイルサイズが期待値と一致するか確認

**書き込みモード**:
- 一括書き込み (1回の `write()` で全データ)
- チャンク分割 (64KB単位で複数回 `write()`)

### ビルド設定

#### [MODIFY] [Makefile](file:///mnt/c/WATCOM/src/os32/Makefile)
- `C_TEST_PROGS` に `e2test` を追加

#### [MODIFY] [deploy.yaml](file:///mnt/c/WATCOM/src/os32/tools/deploy.yaml)
- `/sbin/e2test.bin` をデプロイ対象に追加

## 実行フロー

```text
>> exec e2test.bin

=== ext2 DIND Write Test ===

--- Test 1: New file 280KB (DIND boundary) ---
  write: 286720 bytes in 1 call ... OK
  read back: 286720 bytes ... OK
  verify: 286720/286720 bytes match ... PASS

--- Test 2: Overwrite 280KB ---
  write: 286720 bytes (overwrite) ... OK
  read back: 286720 bytes ... OK
  verify: 286720/286720 bytes match ... PASS

--- Test 3: New file 372632 bytes (original bug size) ---
  write: 372632 bytes in 1 call ... OK
  stat: size=372632 ... OK
  read back: 372632 bytes ... OK
  verify: 372632/372632 bytes match ... PASS

--- Test 4: Chunked write 372632 bytes (64KB chunks) ---
  chunk[0]: 65536 bytes at offset 0 ... OK
  chunk[1]: 65536 bytes at offset 65536 ... OK
  ...
  chunk[5]: 44952 bytes at offset 327680 ... OK
  stat: size=372632 ... OK
  read back: 372632 bytes ... OK
  verify: 372632/372632 bytes match ... PASS

=== Result: 4/4 passed ===
```

## Open Questions

> [!IMPORTANT]
> **メモリ制約**: 外部プログラムのヒープは約4MB。372KBのバッファ × 2 (書き込み用 + 読み戻し用) = 約744KB で問題なし。

> [!NOTE]
> テストファイルのパスは `/tmp/e2test.dat` を使用予定。`/tmp` ディレクトリはext2上に存在する。テスト終了後にファイルを削除する。

## 検証計画

1. `make all` でビルド
2. `/deploy-program` でデプロイ
3. `exec e2test.bin` で実行
4. 全テストが PASS になることを確認
5. カーネルの `[E2W] INCOMPLETE` ログが出力されないことを確認
