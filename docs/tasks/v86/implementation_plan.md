# BIOS HLE NP21/W準拠補完 — 実装計画

[bios_hle_comparison.md](file:///mnt/c/WATCOM/src/os32/docs/tasks/v86/bios_hle_comparison.md) に基づく実装計画。

## 調査結果サマリ

BDA初期化について調査した結果、`v86_mem.c` の `v86_mem_setup()` (L254-407) で **NP21/W準拠の全BDAフィールドが既に正しく設定済み** であることが判明:

| BDAフィールド | アドレス | 設定値 | 設定箇所 |
|-------------|---------|-------|---------|
| SYS_TYPE | 0x0480 | 0x03 (i386) | v86_mem.c:276 ✅ |
| F2HD_MODE | 0x0493 | 0xFF | v86_mem.c:381 ✅ |
| F2DD_MODE | 0x05CA | 0xFF | v86_mem.c:382 ✅ |
| BIOS_FLAG0 | 0x0500 | 0x03 | v86_mem.c:288 ✅ |
| CRT_RASTER | 0x053B | 0x0F | v86_mem.c:340 ✅ |
| DISK_EQUIP | 0x055C | 0x0001 | v86_mem.c:369 ✅ |
| BOOT_DEV | 0x0584 | 0x90 | v86_mem.c:374 ✅ |
| F2DD/F2HD_POINTER | 0x05CC/05F8 | NP21/W値 | v86_mem.c:384-392 ✅ |

→ **BDA初期化の修正は不要**。問題はINT 1Bhのサブ機能実装ギャップに集中。

---

## 修正対象と変更内容

### Step 1: INT 1Bh 欠落サブ機能追加 (`v86_disk.c`)

#### [MODIFY] [v86_disk.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_disk.c)

**1-A. case 0x01 (VERIFY) 追加** (~30行)

NP21/W `bios1b.c` のVERIFYはSEEK + `fdd_read()` ループ (データは破棄)。
OS32 HLEではデータ転送なしで成功を返せば十分。

```c
/* case 0x02 の直前に追加 (L1064付近) */
case 0x01: {
    /* VERIFY: SEEK + データ読み出し(破棄)
     * NP21/W bios1b.c: fdd_read() ループでベリファイするが、
     * HLEではメディア存在確認のみで成功を返す */
    if (func & 0x10) {
        fdc_treg = (u8)(regs[V86_REG_ECX] & 0xFF);
    }
    /* メディア未マウント → エラー */
    if (!v86_disk_is_loop() && !v86_disk_is_phys()) {
        DISK_ERROR_RETURN(log_entry, 0x60, -1, regs);
    }
    log_entry->status = 0x00;
    regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
    regs[V86_REG_EFLAGS] &= ~1UL;
    v86_fdc_sync_seek((u8)(regs[V86_REG_ECX] & 0xFF));
    {
        u8 *disk_int = v86_phys_addr(0x0000, 0x055E);
        *disk_int |= (u8)(0x01 << (regs[V86_REG_EAX] & 0x03));
    }
    break;
}
```

**1-B. case 0x03 (INITIALIZE) `fddbios_equip()` 追加** (~15行)

NP21/W `fddbios_equip()` に準拠したBDA DISK_EQUIP更新を追加:

```c
/* 既存の case 0x03 (L447-451) を拡張 */
case 0x03: {
    /* NP21/W fddbios_equip() 準拠: BDA DISK_EQUIP (0x055C) 更新 */
    u8 *disk_equip = v86_phys_addr(0x0000, 0x055C);
    u8 al = (u8)(regs[V86_REG_EAX] & 0xFF);
    u16 de = (u16)disk_equip[0] | ((u16)disk_equip[1] << 8);
    if (al & 0x80) {
        /* 2HD: 下位4bit = FDD装備ビット (UNIT0のみ=0x01) */
        de &= 0xFFF0;
        de |= 0x0001;
    } else {
        /* 2DD: 上位4bit */
        de &= 0x0FFF;
        de |= 0x1000;
    }
    disk_equip[0] = (u8)(de & 0xFF);
    disk_equip[1] = (u8)(de >> 8);
    regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
    regs[V86_REG_EFLAGS] &= ~1UL;
    log_entry->status = 0x00;
    break;
}
```

**1-C. case 0x0E (密度設定) 追加** (~15行)

```c
/* case 0x0A の後に追加 */
case 0x0E: {
    /* NP21/W bios1b.c 準拠: F2HD_MODE/F2DD_MODE BDA更新 */
    u8 al = (u8)(regs[V86_REG_EAX] & 0xFF);
    u8 *fmode;
    if (al & 0x80) {
        fmode = v86_phys_addr(0x0000, 0x0493); /* F2HD_MODE */
    } else {
        fmode = v86_phys_addr(0x0000, 0x05CA); /* F2DD_MODE */
    }
    if (func & 0x80) {
        /* 密度設定 */
        *fmode &= 0x0F;
        *fmode |= (u8)((func & 0x0F) << 4);
    } else {
        /* 面設定 */
        *fmode &= 0xF0;
        *fmode |= (u8)(func & 0x0F);
    }
    regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
    regs[V86_REG_EFLAGS] &= ~1UL;
    log_entry->status = 0x00;
    break;
}
```

**1-D. case 0x02 (診断読み) → READフォールスルー** (~5行)

既存の「ack応答のみ」から、case 0x01/0x06 の READ 処理にフォールスルーさせる:

```c
/* 既存の case 0x02 (L1064-1070) を書き換え */
case 0x02:
    /* 診断読み: NP21/W ではREADと同一処理 (ARS対策のみ差異)
     * OS32ではREADにフォールスルー */
    /* FALLTHROUGH */
```

---

### Step 2: INT 18h 微修正 (`v86_bios.c`)

#### [MODIFY] [v86_bios.c](file:///mnt/c/WATCOM/src/os32/kernel/v86_bios.c)

**2-A. AH=16h VRAM初期化: DL/DHパラメータ対応** (~10行)

現在 `tvram_clear_all()` (空白+白色固定) だが、NP21/W は DL=文字コード、DH=アトリビュート:

```c
/* 既存の AH=16h 処理を修正 */
case 0x16: {
    /* NP21/W bios0x18_16(DL, DH) 準拠 */
    u8 fill_ch = (u8)(regs[V86_REG_EDX] & 0xFF);         /* DL */
    u8 fill_at = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);  /* DH */
    if (fill_ch == 0) fill_ch = 0x20; /* デフォルト: 空白 */
    if (fill_at == 0) fill_at = 0xE1; /* デフォルト: 白色 */
    tvram_clear_with(fill_ch, fill_at); /* 新関数 */
    break;
}
```

`tvram_clear_with(u8 ch, u8 attr)` をヘルパーとして追加。`tvram_clear_all()` はこれのラッパーに変更。

**2-B. AH=17h/18h ブザーON/OFF** (~5行)

```c
case 0x17:
    outp(0x37, 0x06); /* ブザーON */
    break;
case 0x18:
    outp(0x37, 0x07); /* ブザーOFF */
    break;
```

---

## 修正しないもの (理由)

| 項目 | 理由 |
|------|------|
| BDA初期化 | 既にNP21/W準拠で完了済み |
| INT 18h AH=0Ah GDCモード | DOSブートでは不使用 (ack応答で十分) |
| INT 18h 40h-49h グラフィック | DOSブートでは不使用 |
| INT 18h 1Ah ユーザ文字定義 | ゲーム用、P3優先度 |
| INT 1Ch 02h/03h インターバルタイマ | ゲーム用、P3優先度 |
| INT 09h キーボード | IRQ注入方式で動作済み |
| FDCステートマシン同期 | 既にNP21/W準拠で実装済み |

---

## 検証計画

### ビルド & デプロイ

`/build-os32` ワークフローを使用:

```bash
# Step 1: カーネルビルド (v86_disk.c, v86_bios.c はカーネルモジュール)
make -C /mnt/c/WATCOM/src/os32 kernel

# Step 2: HostDrvデプロイ + NHD同期 + NP21/W再起動
# (ワークフロー steps 3-7)
```

### テスト手順

1. **ビルド成功確認**: `make kernel` がエラーなく完了
2. **NP21/W再起動 → OS32ブート**: `ver` コマンドで応答確認
3. **V86 FreeDOSブート**: `v86 freedos.fdi` で FreeDOS セッション開始
4. **スクリーンショット取得**: `curl -sX GET http://localhost:8032/screenshot > screenshot.png`
5. **ブート進行確認**: FreeDOS IPL→IO.SYS→コマンドプロンプト到達を目視確認

### デバッグ手段

**v86_diag.log** (最重要): V86セッション中にF12キーを押して終了すると、`/host/debug/v86_diag.log` (ホスト側: `C:\os32\debug\v86_diag.log`) に体系化された診断ログが自動出力される。

ログに含まれるセクション:
- `[SESSION]` — ブートモード・イメージパス・タイムアウト設定
- `[EXIT]` — 終了理由・最終CS:IP・実行時間
- `[GUEST MEMORY STATE]` — BDA (タイマ/DISK_INT/DISK_EQUIP/ブートフラグ/IVT)
- `[GP HANDLER]` / `[IRQ0]` — #GP回数・INT統計・IRQ0注入カウンタ
- `[PIC]` / `[PIT]` / `[FDC]` — 仮想ハードウェア状態・MSRリードログ・SENSE INT統計
- `[DISK I/O LOG]` — INT 1Bh 全呼び出し (最新64件, AH/CHS/ステータス)
- `[GP TRACE]` — #GPトレース (最新1024件, tick/CS:IP/INT#/AX/CX)
- `[MEMSW]` — メモリスイッチ init/exit 差分
- `[IVT DIFF]` — IVTベクタ変更差分 (ゲストがフックしたINT番号)

テスト後のログ取得手順:
```bash
# 1. V86セッション起動
curl -sX POST http://localhost:8032/cmd -d "v86 freedos.fdi"

# 2. ブートを観察 (スクリーンショット)
curl -sX GET http://localhost:8032/screenshot > screenshot.png

# 3. F12キーでV86セッション終了 → v86_diag.log自動生成
curl -sX POST http://localhost:8032/key -d "F12"

# 4. ログを取得 (HostDrv経由)
cat /mnt/c/os32/debug/v86_diag.log
```

その他の手段:

| 手段 | 用途 |
|------|------|
| シリアルログ (`v86_debug_enabled`) | INT 1Bh 呼び出しパラメータ・BDA状態のリアルタイム確認 |
| NP21/Wスクリーンショット | ゲスト画面の目視確認 (`/screenshot` API) |
| `/analyze-v86-logs` ワークフロー | v86_diag.log の後分析 (NP21/W再起動不要) |

### 回帰テスト

- 既存のV86テスト (`v86_test.c`) がパスすること
- 既存のFDDイメージ (D88/FDI/RAW) の読み書きが正常なこと

---

## 作業順序

1. **Step 1-A〜D**: `v86_disk.c` に VERIFY/INITIALIZE/密度設定/診断読みを追加
2. **Step 2-A〜B**: `v86_bios.c` に VRAM初期化パラメータ対応+ブザーを追加
3. **ビルド**: `make kernel` でコンパイル確認
4. **テスト**: `/build-os32` ワークフローでデプロイ・動作確認
