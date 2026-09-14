# 票 S6-P — ext2 の小さな書き込みが極端に遅い (診断と処置)

対象の観測: [`TASK_S6.md`](TASK_S6.md) 「PM 受入記録」 (2026-09-14)
実測の記録: [`tools/tests/s6p_tdd.md`](../../../tools/tests/s6p_tdd.md)
基点: `4fd5fb4` (feat/gui)

`tar c /tmp/e8.tar /etc/system.cfg` (15B のファイル 1 本 → 書庫 2048B) が
ext2 (hd0) で 15 秒を超え、HostDrv では 0.3 秒。

## 1. 原因は 2 段重ね

**掛け算の形になっている。**

| 段 | 何 | 倍率 |
|---|---|---|
| A (呼び出し側) | `lib/microtar/microtar.c` の `write_null_bytes()` が **1 バイトずつ** `sys_write` する。2KB の書庫に **1523 回** | 512B 単位で書けば 4〜6 回 → **約 300 倍** |
| B (ext2 側) | `sys_write` 1 回の固定費が **26 セクタ** (サイズに関係なく) | 妥当な下限は 10 セクタ → **2.6 倍** |

1523 × 26 = **39602 セクタ**。NP21/W の PIO は実測 ≈0.38ms/セクタなので約 15 秒 —
観測とぴったり合う。`/api/status` の EIP が `ide_*_sector_chs` に集中していたのも、
「遅い処理」ではなく「I/O の回数が異常」の像。

### A の現物

`lib/microtar/microtar.c:111`

```c
static int write_null_bytes(mtar_t *tar, int n) {
  int i, err;
  char nul = '\0';
  for (i = 0; i < n; i++) {
    err = twrite(tar, &nul, 1);      /* ← 1 バイトずつ tar->write を呼ぶ */
    if (err) return err;
  }
  return MTAR_ESUCCESS;
}
```

呼び出し元は 2 か所:
- `mtar_write_data()` — データのあとを 512B 境界まで埋める (15B のファイルなら **497 回**)
- `mtar_finalize()` — 終端の 2 レコード = **1024 回**

`userland/cmds/tar.c` の `mt_write` → `io_write` → `g_api->sys_write` なので、
この 1521 回がそのまま KAPI 呼び出しになる。
HostDrv で速いのは 1 回の書き込みが安いから (ホスト側の 1 write システムコール) で、
回数の異常は同じ。

### B の内訳 (`sys_write` 1 回 = 26 セクタ = 13 ブロック I/O)

`fs/vfs_fd.c` の `vfs_write_fd()` は FD ではなく **パス文字列**を FS に渡す
(`ops->write_stream(fs_ctx, f->path, buf, size, f->offset)`)。だから毎回ここから始まる:

| 経路 | ブロック読 | ブロック書 |
|---|---|---|
| `ext2_vfs_write_stream` → `ext2_resolve_path` → `ext2_lookup("/tmp/e8.tar")`<br>`find_entry(root,"tmp")` = inode 1 + dir ブロック 1、`find_entry(tmp,"e8.tar")` = 同 | 4 | 0 |
| `ext2_write_stream` → `ext2_read_inode` | 1 | 0 |
| 部分ブロックの read-modify-write | 1 | 1 |
| `ext2_write_inode` (read-modify-write) | 1 | 1 |
| `ext2_sync` → `ext2_write_super_raw` (ブロック 1) | 1 | 1 |
| `ext2_sync` → `ext2_write_gd_raw` (ブロック 2) | 1 | 1 |
| **合計** | **9** | **4** |

1 ext2 ブロック (1KB) = 512B セクタ 2 本 (`fs/ext2_super.c` の
`ext2_read_block` / `ext2_write_block` が 2 回に割る) → 13 × 2 = 26 セクタ。

後方 `lseek` は無実 — `fs/vfs_fd.c` の `vfs_seek()` は `VfsFile.offset` を
書き換えるだけで FS を一切触らない (0 セクタ、試験 P4 で表明)。
create 経路には backward seek 自体が出ない (`mtar_seek(last_header)` は read 側)。

## 2. B は直した (この票の範囲)

`fs/ext2_ctx.h` / `ext2_super.c` / `ext2_inode.c` / `ext2_dir.c` / `ext2_vfs.c` /
`ext2_priv.h`。**write-through の契約は変えていない** (書き込みが戻った時点で
ディスクに反映されている)。

1. **`ext2_sync()` は SB / GD が動いていなければ何もしない** (`Ext2Ctx.meta_dirty`)。
   空き数・`used_dirs` を動かす 6 か所が `ext2_meta_touch()` を呼ぶ。
   書き戻すものが無いときに出していた同じ中身の read-modify-write
   4 ブロック (8 セクタ) が消える。
2. **解決済み経路の記憶** (`Ext2Ctx.memo[4]` + `ns_gen`)。
   `ext2_add_entry` / `ext2_delete_entry` が成功したら世代が進み、記憶は全部無効。
   パスが別の inode を指すようになる操作 (create / unlink / rename / mkdir / rmdir)
   は必ずそのどちらかを通る。`ext2_lookup` のディレクトリ辿り直し 4 ブロック
   (8 セクタ) が消える。
3. 付随: `sizeof(Ext2Ctx)` が 732 → 1788 バイトになるので、`fs/ext2_fmt.c` が
   スタックに積んでいた一時コンテキストを `static` にした
   (`ext2_format()` のフレーム 824 → 88 バイト — 変更前より小さい)。
   マウント中の `Ext2Ctx` は今までどおり kmalloc。

結果 (`python3 -B tools/tests/test_ext2_write_io.py`、10/10 PASS):

| | 前 | 後 |
|---|---|---|
| `sys_write(1B)` / `sys_write(512B)` (既存ブロック) | 26 セクタ | **10 セクタ** |
| `sys_write(512B)` (新ブロック確保あり) | 28 セクタ | 28 セクタ (変わらず — 確保があれば sync は要る) |
| `sys_read(512B)` | 12 セクタ | **4 セクタ** |
| `tar c` 全体 (microtar のまま 1523 回) | 39602 セクタ | **15258 セクタ** |
| `tar c` 全体 (512B 単位 4 回で書いた場合) | 108 セクタ | **68 セクタ** |

ext2 だけで **2.6 倍**。15 秒 → 約 5.8 秒の見込み。

## 3. A は PM へ — 残りの本丸 (推奨)

`lib/microtar/` と `userland/cmds/tar.c` はこの worktree (基点 `4fd5fb4`) に
まだ入っていない (PM の作業ツリーの未コミット分) ため、コーダー側では触っていない。

### 直し方 (最小、microtar の外部仕様は変えない)

`lib/microtar/microtar.c` の `write_null_bytes()` をブロック単位にする。
OS32 版には既に「OS32 追加」の節があるので、そこと同じ体裁で:

```c
/* OS32: 元は 1 バイトずつ twrite していた。ext2 は sys_write 1 回につき
 * 10 セクタ前後の固定費があるので、15B のファイル 1 本でも padding だけで
 * 1521 回 = 15000 セクタ以上になり、15 秒を超えていた (票 S6-P)。 */
static int write_null_bytes(mtar_t *tar, int n) {
  static const char nul[512];        /* .bss = 全部 0 */
  int err;
  while (n > 0) {
    int chunk = (n > (int)sizeof(nul)) ? (int)sizeof(nul) : n;
    err = twrite(tar, nul, (unsigned)chunk);
    if (err) return err;
    n -= chunk;
  }
  return MTAR_ESUCCESS;
}
```

- `twrite()` は `tar->pos += size` を自分でやるので、位置の勘定は変わらない。
- `static const char nul[512]` は .bss (`const` なので .rodata) の 512B。
  外部プログラムのスタックは細いので、自動変数にしないこと。
- C89 ([C1]) — 宣言はブロック先頭、`//` を使わない。

これで `tar c /tmp/e8.tar /etc/system.cfg` の `sys_write` は 1523 → 4 回、
I/O は **68 セクタ ≈ 0.03 秒** になる。

### ついでに見ておく値

`userland/cmds/tar.c` の `TAR_CHUNK` (本体データの読み書き単位)。
512B の倍数で、できれば 4KB 以上にしておくと展開側も同じ倍率で効く。

## 4. 手を出さなかった案 (規模の見積り付き)

| 案 | 効き | 規模 | 見送りの理由 |
|---|---|---|---|
| `ext2_write_stream` の `ext2_write_inode` が inode テーブルブロックを読み直すのを省く (直前の `ext2_read_inode` が同じブロックを `ext2_g_blk` に置いている) | 10 → 8 セクタ (-20%) | 小 (`ext2_g_blk` の保持ブロックを追う 20 行 + 無効化点の洗い出し) | 共有バッファの取り違えは過去に 2 度ファイルを壊している (gotcha §4-24 / §4-32)。効きの割に危ない。やるなら `ext2_read_block` / `ext2_write_block` の中だけで (ctx, block) を記録し、`ext2_mount` の生 memcpy で必ず無効化する |
| inode が 1 バイトも変わっていなければ `ext2_write_inode` ごと省く | 上書き専用で 10 → 6 セクタ | 小 (10 行) | `ext2_current_time()` が定数なので「サイズが伸びない上書き」には効く。tar の create はサイズが毎回伸びるので今回は効かない。`cp` の上書きや SQLite のページ書きには効く |
| ブロックキャッシュ層 (write-back) | 大 | 大 | write-through の契約が変わる。電源断・リセットの扱いを決め直す必要があり、この票の範囲外 |
| `ext2_read_block` / `ext2_write_block` が 2 セクタを 1 回の `dev_blk_*_lba(.., 2, ..)` で出す | 0 | 小 | `dev_blk_read_lba` は中でセクタごとに `blk_read_chs` を回すので、IDE の呼び出し回数は同じ。効果なし |
| `fs/vfs_fd.c` が FD にパスではなく inode を持つ | (2) の記憶と同じ | 中 (VfsOps の契約変更、全 FS ドライバ) | 記憶で同じ効きが取れたので不要。将来 VfsOps を触るときに検討 |

## 5. 残り: ゲストでの確認 (PM)

1. `make clean` は不要 (KAPI は触っていない)。`make kernel` → `make deploy-kernel`
   ([D1] NP21/W 停止)。
2. §3 の microtar 修正を入れたら `make external` (tar は userland/cmds)。
3. 期待値:
   - microtar 修正**なし**で `tar c /tmp/e8.tar /etc/system.cfg` → 15 秒超 → **約 6 秒**
     (`/api/cmd` のタイムアウトは 15s なので通るはず、[V3] は守ること)
   - microtar 修正**あり** → **1 秒未満**
4. `tar t` / `tar x` で書庫の中身が今までどおりであること。
5. 回帰の目: `cp` / `cat >` / SQLite (`cfg` コマンド) が壊れていないこと。
   ext2 の空き数がずれると `df` が合わなくなるので、書き込みのあと `df` と
   再起動後の `df` を突き合わせるのが早い。
