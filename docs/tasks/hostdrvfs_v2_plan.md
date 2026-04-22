# HostDrvFS v2 実装計画

## 背景と目的

HostDrvFS v1のREAD IRP失敗の原因を、NTゲストドライバソース (`np2tool/hostdrvnt/hostdrv.c`) と
エミュレータ側ソース (`generic/hostdrvnt.c`) の徹底比較から特定し、修正した v2 を実装する。

### 環境情報

- NP21/W: np21x64w.exe (x86-64ビルド)
- CPU設定: cpu_fami=3 (i386)
- HAXM: **無効** (INIに設定なし、ソフトウェアエミュレーション)
- HostDrv設定: `usehdrvn=true`, `hdrvroot=C:\os32`, `hdrv_acc=7`

### v1で動作したもの

| IRP操作 | 結果 |
|---------|------|
| 検出 (`inp 0x7EC/0x7EE`) | ✅ 成功 |
| `IRP_MJ_CREATE` | ✅ 成功 |
| `IRP_MJ_DIRECTORY_CONTROL` (`ls /host`) | ✅ 成功 |
| `IRP_MJ_QUERY_INFORMATION` (`ls -l /host`) | ✅ 成功 |
| `IRP_MJ_CLEANUP` / `IRP_MJ_CLOSE` | ✅ 成功 |
| VFSマウント (`/host`) | ✅ 成功 |
| UTF-8 ↔ UTF-16LE変換 | ✅ 成功 |

### v1で動作しなかったもの

| 方式 | 結果 |
|------|------|
| `invoke_clear()` + fileIndex復元 | `0xC000000D` (INVALID_PARAMETER) |
| `g_stack` のみ memset + fileObject再設定 | `0xDEADBEEF` (SENTINEL) |
| フィールド上書きのみ (memsetなし) | `0xDEADBEEF` (SENTINEL) |
| mfence追加 | `0xC000000D` (INVALID_PARAMETER) |

---

## NTゲストドライバ vs v1 差異分析

NTゲストドライバの `HostdrvDispatch()` (hostdrv.c L503-790) が
エミュレータに渡す情報と、v1がエミュレータに渡す情報を比較。

### InvokeInfo (HOSTDRV_INFO) レイアウト

| Offset | NTドライバ | v1 | 状態 |
|--------|-----------|-----|------|
| 0x00 | `PIO_STACK_LOCATION stack` (ポインタ) | `stackAddr` (u32) | ✅ |
| 0x04 | `PIO_STATUS_BLOCK status` (ポインタ) | `statusAddr` (u32) | ✅ |
| 0x08 | `PVOID systemBuffer` | `inBufferAddr` | ✅ |
| 0x0C | `ULONG deviceFlags` = `irpSp->DeviceObject->Flags` | **0** (未設定) | ⚠️ |
| 0x10 | `PVOID outBuffer` (MdlAddress/SystemBuffer/UserBuffer) | `g_databuf` | ✅ |
| 0x14 | `PVOID sectionObjectPointer` | **0** (未設定) | ⚠️ |
| 0x18 | `ULONG version = 4` | `version = 1` | ⚠️ 後述 |

### 重大な差異

#### 差異1: `deviceFlags` が未設定 (v1: 常に0)

NTドライバ:
```c
lpHostdrvInfo->deviceFlags = irpSp->DeviceObject->Flags;
/* 通常は DO_BUFFERED_IO (0x10) | FILE_DEVICE_IS_MOUNTED (0x20) = 0x30 */
```

v1: `g_invoke.deviceFlags = 0` (invoke_clearでゼロのまま)

エミュレータ側で `deviceFlags` を直接参照する箇所は現時点で確認できないが、
NTドライバが意図的に設定している値。

#### 差異2: `sectionObjectPointer` が未設定 (v1: 常に0)

NTドライバ:
```c
lpHostdrvInfo->sectionObjectPointer = irpSp->FileObject->SectionObjectPointer;
/* MiniSOP_GetSOP() で割り当てたNonPagedPoolメモリ */
```

v1: `g_invoke.sectionObjectPointerAddr = 0`

エミュレータのREAD処理ではSOPを直接参照しないため、直接原因ではない。

#### 差異3: version=1 vs version=4 → `s_fsContextUserDataOffset`

- NTドライバ: `version = 4` → `s_fsContextUserDataOffset = 40`
- v1: `version = 1` → `s_fsContextUserDataOffset = 0`

v1はversion=1でFsContext先頭にfileIndexを配置。理論的には正しい。

#### 差異4: ⭐ NTドライバのCREATE前処理

NTドライバはCREATE時に以下の前処理を行う (hostdrv.c L522-553):

```c
/* 1. FsContext をNonPagedPoolから64バイト確保 */
irpSp->FileObject->FsContext = ExAllocatePoolWithTag(NonPagedPool,
    sizeof(HOSTDRV_FSCONTEXT), "HSFC");
RtlZeroMemory(irpSp->FileObject->FsContext, sizeof(HOSTDRV_FSCONTEXT));

/* 2. SectionObjectPointer を取得 */
sopIndex = MiniSOP_GetSOPIndex(irpSp->FileObject->FileName);
irpSp->FileObject->SectionObjectPointer = MiniSOP_GetSOP(sopIndex);
```

v1はFsContext/SOPをグローバル変数で擬似的に確保。
問題は、NTドライバでは**OSが各IRP呼び出しごとにFileObjectを自動管理**するのに対し、
v1は同一グローバル変数を使い回す点。

#### 差異5: ⭐⭐⭐ invoke_clear() の破壊的動作 (根本原因)

**NTドライバの動作**:
```
IRP_MJ_CREATE → HostdrvDispatch(Irp)
  ↓ 引数のIrpからFileObjectを取得 (OS管理)
  ↓ FsContextを確保、ハイパーコール
  ↓ エミュレータがFsContextにfileIndexを書き込み
  ↓ FileObject/FsContextはOS管理のメモリに永続化

IRP_MJ_READ → HostdrvDispatch(Irp)
  ↓ 引数のIrpから同じFileObjectを取得 (OS管理)
  ↓ FileObject.FsContext → 前回のfileIndex保持済み
  ↓ ハイパーコール → READ成功
```

**v1の動作**:
```
hdrv_read_stream():
  invoke_clear()  → g_fobj/g_fsctx を全クリア
  hostdrv_create() → CREATE成功、g_fsctx.fileIndex 設定済み
  hostdrv_read()   → g_stack のみ上書き (invoke_clear呼ばない)
                   → g_fsctx.fileIndex は保持されている(はず)
                   → ハイパーコール → ❌ INVALID_PARAMETER
```

**問題の核心**: `hostdrv_read()` が `g_stack` のみ上書きする方式では、
`g_stack` 内の `control` フィールドや `deviceObject` フィールドに
CREATE時の古い値が残っている。NTドライバはOSが毎回新しい IO_STACK_LOCATION
を割り当てるため、このような汚染は起きない。

**解決策**: READ時に `g_stack` を**全クリア** + 必要フィールドのみ再設定。
特に `g_stack.fileObject = (u32)&g_fobj` の再設定が必須（全クリアで消えるため）。

---

## v2 設計方針

### 原則: NTドライバの動作を忠実に模倣する

- NTドライバが設定しないフィールド → 設定しない（ゼロのまま）
- NTドライバが設定するフィールド → **全て**設定する（マジックナンバーでも可）
- NTドライバが保持するもの → 保持する（invoke_clear で壊さない）

### 構造変更

#### 1. `invoke_clear()` の廃止 → `session_begin()` に置き換え

```c
/* SectionObjectPointers ダミー (12バイト) */
static u32 g_sop[3] __attribute__((aligned(4)));

/* セッション開始 (CREATE前に1回だけ呼ぶ) */
static void session_begin(void)
{
    kmemset(&g_invoke, 0, sizeof(g_invoke));
    kmemset(&g_stack, 0, sizeof(g_stack));
    kmemset(&g_fobj, 0, sizeof(g_fobj));
    kmemset(&g_fsctx, 0, sizeof(g_fsctx));
    kmemset(&g_secctx, 0, sizeof(g_secctx));
    kmemset(&g_iostatus, 0, sizeof(g_iostatus));
    kmemset(g_sop, 0, sizeof(g_sop));

    /* InvokeInfo 固定フィールド */
    g_invoke.stackAddr = (u32)&g_stack;
    g_invoke.statusAddr = (u32)&g_iostatus;
    g_invoke.inBufferAddr = (u32)g_databuf;
    g_invoke.outBufferAddr = (u32)g_databuf;
    g_invoke.sectionObjectPointerAddr = (u32)g_sop;
    g_invoke.deviceFlags = 0x30; /* DO_BUFFERED_IO | FILE_DEVICE_IS_MOUNTED */
    g_invoke.version = 1;

    /* FILE_OBJECT 固定フィールド */
    g_fobj.FsContext = (u32)&g_fsctx;
    g_fobj.SectionObjectPointer = (u32)g_sop;
    g_fobj.Flags = NP2_FO_SYNCHRONOUS_IO;

    /* IO_STACK_LOCATION 固定フィールド */
    g_stack.fileObject = (u32)&g_fobj;
}
```

#### 2. IRP別のセットアップ関数

```c
/* g_stack を全クリアして READ 用に再設定 */
static void setup_read(u32 length, u64 offset)
{
    kmemset(&g_stack, 0, sizeof(g_stack));
    g_stack.majorFunction = NP2_IRP_MJ_READ;
    g_stack.fileObject = (u32)&g_fobj;  /* ★ 再設定必須 */
    g_stack.parameters.read.length = length;
    g_stack.parameters.read.byteOffset = offset;

    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;
}

/* g_stack を全クリアして CLEANUP 用に再設定 */
static void setup_cleanup(void)
{
    kmemset(&g_stack, 0, sizeof(g_stack));
    g_stack.majorFunction = NP2_IRP_MJ_CLEANUP;
    g_stack.fileObject = (u32)&g_fobj;
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;
}

/* g_stack を全クリアして CLOSE 用に再設定 */
static void setup_close(void)
{
    kmemset(&g_stack, 0, sizeof(g_stack));
    g_stack.majorFunction = NP2_IRP_MJ_CLOSE;
    g_stack.fileObject = (u32)&g_fobj;
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;
}
```

#### 3. VFS呼び出しパターンの変更

```
v2 hdrv_read_stream:
  session_begin()       → 全バッファ初期化 (1回だけ)
  setup_create(path)    → g_stack にCREATE設定
  hostdrv_hypercall()   → CREATE 実行
  setup_read(len, off)  → g_stack を全クリア + READ設定
                          (g_fobj / g_fsctx / g_invoke は不変)
  hostdrv_hypercall()   → READ 実行
  setup_cleanup()       → g_stack を全クリア + CLEANUP設定
  hostdrv_hypercall()
  setup_close()         → g_stack を全クリア + CLOSE設定
  hostdrv_hypercall()
```

**ポイント**:
- `session_begin()` はCREATEの前に1回だけ呼ぶ
- 後続のIRPでは `g_stack` のみを**全クリア+再設定**する
- `g_fobj`, `g_fsctx`, `g_invoke` は**CREATE以降触らない**
- `g_stack.fileObject = (u32)&g_fobj` は毎回再設定する

---

## 提案する変更ファイル

| ファイル | 操作 | 内容 |
|---------|------|------|
| `fs/hostdrvfs.c` | 復元+全面改修 | session_begin/setup_xxx方式に書き換え |
| `fs/hostdrvfs.h` | 復元 | 変更なし |
| `fs/hostdrvfs_proto.h` | 復元 | 変更なし |
| `lib/kutf16.c` | 復元 | 変更なし |
| `lib/kutf16.h` | 復元 | 変更なし |
| `Makefile` | 修正 | hostdrvfs.c, kutf16.c をソースリストに追加 |
| `kernel/kernel.c` | 修正 | hostdrvfs 自動マウント処理を復元 |

---

## 検証計画

### Phase 1: CREATE + QUERY_INFORMATION (既存動作の確認)

1. `make clean` → `make all`
2. `/host` マウント確認
3. `ls /host` でディレクトリ列挙
4. ファイルサイズ取得

### Phase 2: READ (今回の主目的)

5. `cat /host/test.txt` でファイル読み出し
6. SENTINEL / INVALID_PARAMETER が出ないことを確認

### Phase 3: デバッグ (Phase 2 失敗時)

7. READ前後の `g_fsctx.fileIndex` を kprintf で表示
8. `g_stack` を hexdump して全フィールド確認

---

## 関連ファイル

| ファイル | 内容 |
|---------|------|
| `_backup/hostdrvfs_v1/` | v1バックアップ (復元元) |
| `np21w-src-main/generic/hostdrvnt.c` | エミュレータ側処理 (UTF-8変換済み) |
| `np21w-src-main/generic/hostdrvntdef.h` | エミュレータ側構造体定義 |
| `np21w-src-main/np2tool/hostdrvnt/hostdrv.c` | NTゲスト側ドライバ |

---

## 完了報告と発見事項 (Phase 1 & Phase 2)

**ステータス**: ✅ **完了**

Phase 1 (CREATE/列挙) および Phase 2 (READ) の動作が完全に成功し、`0xC000000D` エラーやマウント・列挙失敗などの問題がすべて解消しました。

### 主要な発見と修正点

1. **コンパイラ最適化による通信同期不良 (SENTINEL問題)**:
   - v2のセッションベース実装後、エミュレータが応答しない(`SENTINEL`のまま)事象が発生しました。
   - 原因は、通信バッファ (`g_invoke`, `g_iostatus`, `g_stack` など) に `volatile` 修飾子が付与されていなかったためでした。`io.h` の `outp` 関数に `"memory"` clobber バリアが無かったため、GCCの最適化によってメモリの再読み込みが省略され、レジスタにキャッシュされた古い `SENTINEL` 値が評価されていました。
   - 対策として、通信バッファの定義に `volatile` を追加し、コンパイラによる最適化を抑制しました。
2. **初期化リセットシーケンスの重要性**:
   - NTゲストドライバの挙動に合わせ、`hostdrvfs_detect` 時に `0x00000000` アドレスと `HDR9801` を送信するリセット処理を追加しました。これにより、NP21/W側の状態異常による不整合を防ぐことができます。
3. **READ IRP の成功**:
   - `volatile` 追加とセッション保持方式への移行により、エミュレータとの通信同期が確立し、`cat /host/test.txt` によるファイルの読み出しが正常に動作することが確認できました。

これにより、OS32とホストマシン (Windows) 間のファイル共有(読み取り)が安定して利用可能になりました。

---

## 完了報告と発見事項 (Phase 3: 書き込み系操作の実装)

**ステータス**: ✅ **実装完了 (検証待ち)**

Phase 1/2 の安定した読み取り基盤の上に、ファイルの書き込みやディレクトリ操作等の変更系操作 (Phase 3) を実装しました。

### 実装した機能
1. **WRITE IRP (`NP2_IRP_MJ_WRITE`)**:
   - チャンク単位でのファイル書き込み `hostdrv_write()` を実装。
2. **SET_INFORMATION IRP (`NP2_IRP_MJ_SET_INFORMATION`)**:
   - 以下の操作をエミュレータ側へ伝えるための `hostdrv_set_info()` を実装。
     - **ファイル削除 (`unlink`, `rmdir`)**: `NP2_FileDispositionInformation` で `DeleteFileOnClose` フラグを設定。
     - **ファイル名変更 (`rename`)**: `NP2_FileRenameInformation` で移動先パスを指定。
     - **ファイルサイズ切り詰め (EOF設定)**: `NP2_FileEndOfFileInformation` を使用。
3. **VFSスタブの置き換え**:
   - `hdrv_write_file`, `hdrv_mkdir`, `hdrv_rmdir`, `hdrv_unlink`, `hdrv_rename`, `hdrv_write_stream` のスタブ関数を実際の実装で置き換えました。

### 構造体レイアウトへの配慮
- `hostdrvntdef.h` の定義をもとに、`Np2FileRenameInformation` などの構造体を `hostdrvfs_proto.h` に追加しました。
- GCCの `__attribute__((packed))` とアラインメントに注意し、Windows API 側の `pack(8)` 相当となるようにパディングを調整しました。

**次のステップ**: 実機(エミュレータ)で書き込み、ディレクトリ作成、削除、リネームが正しく動作するかどうかの統合テストを行います。
