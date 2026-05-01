# V86 FDDテスト — 現状まとめ

## 目的

OS32カーネルから独立した最小アセンブリプログラムで、以下のV86モード実行パスを**FDDブートで証明**する:

```
リアルモード → プロテクトモード → V86モード → HLT(#GP) → PMに復帰
```

成功すれば、このロジックをOS32カーネル本体に適用し、DOS互換レイヤー（VZ Editor実行等）の基盤とする。

---

## 達成済み ✅

| ステップ | 状態 | 根拠 |
|----------|------|------|
| FDDブートセクタ起動 | ✅ | 画面に「BOOT」表示 |
| BPB付きブートセクタ | ✅ | boot_fat.asm準拠 |
| INT 1Bh FDDリード | ✅ | 画面に「L」表示、セクタ1ロード成功 |
| GDTロード (動的ベース計算) | ✅ | `gdtr=0001fd20:001f` (クラッシュダンプで確認) |
| プロテクトモード移行 | ✅ | 画面に「PM」表示 |
| TSS設定・LTR | ✅ | `tr=0018(00070000:0067)` (クラッシュダンプで確認) |
| IDT設定・LIDT | ✅ | `idtr=00060000:07ff` (クラッシュダンプで確認) |
| ページング有効化 | ✅ ※ | 画面に「G」が未表示だが、コードフロー的に「V」が表示されているので通過はしている |
| V86モード遷移 (IRETD) | ✅ ※ | V86コードが実行された兆候あり（#GPが発生） |

## 行き詰まっている点 ❌

### 1. #GPハンドラ内でのDS=0によるトリプルフォルト

**現象:** V86モードのHLT命令で#GPが発生し、`isr_gp`ハンドラに遷移するが、CPUがDS/ES/FS/GSをゼロクリアするため、ハンドラ内の`movzx ecx, byte [eax]`（オペコードフェッチ）がNULLセグメントアクセスとなりネスト#GP → トリプルフォルト。

**対策（最新版で適用済み、未テスト）:** `isr_gp`先頭で`DS=0x10`を即座に復元するコードを追加。

```nasm
isr_gp:
    cli
    push    eax
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    pop     eax
    test    dword [esp + 12], 0x020000  ; V86フラグ確認
    jnz     .v86_gp
```

> [!IMPORTANT]
> この修正は最新のD88に反映済み（NP21/Wディレクトリにコピー済み）だが、**まだテスト実行されていない**。

### 2. V86→PM復帰時のスタック構造

V86モードから#GPが発生した場合、CPUはスタックに以下を積む:

```
[ESP+0]  Error Code (通常0)
[ESP+4]  EIP
[ESP+8]  CS
[ESP+12] EFLAGS (VM=1)
[ESP+16] ESP (V86)
[ESP+20] SS (V86)
[ESP+24] ES
[ESP+28] DS
[ESP+32] FS
[ESP+36] GS
```

現在のコードは `[esp+4]` を EIP、`[esp+8]` を CS として参照しているが、**Error Codeの有無でオフセットがずれる可能性**がある。#GP(13)はError Codeを伴う例外なので、正しくは:
- `[esp+0]` = Error Code
- `[esp+4]` = EIP
- `[esp+8]` = CS  
- `[esp+12]` = EFLAGS

→ 現在のコードは正しい（Error Code=0がpush済み）。

### 3. 「G」(ページング)表示が欠落

画面上「G」が表示されないが、コードフロー上「V」が表示されているため、ページング自体は通過している可能性が高い。ただし:

- ページテーブルが正しく設定されていないと、V86コードのTVRAM書き込み(0xA0000)でページフォルトが発生する可能性
- PDE[0]は0-4MB領域をカバーするが、PTエントリは256個(1MB)のみ。0xA0000は範囲内(entry #160)

---

## テスト環境の課題

### FDDブート手順が手動

`start_np21w.sh` はD88をコピーしてNP21/Wを起動するが、**ブート順序がHDD優先**のため、手動でFDD1にD88をセットしてリセットする必要がある。

### ビルド→テストのサイクル

```bash
# 1. アセンブル
nasm -f bin tests/v86_fdd_test.asm -o /tmp/v86_test.bin

# 2. D88変換
python3 tools/mkd88.py /tmp/v86_test.bin images/v86_test.d88

# 3. NP21/Wディレクトリにコピー
cp images/v86_test.d88 "/mnt/c/Users/hight/OneDrive/ドキュメント/np21w/v86_test.d88"

# 4. 手動: NP21/W → FDD1 → v86_test.d88 → Reset
```

---

## 次のステップ

1. **DS復元修正のテスト実行** — 最新D88でリセットし、「PASS」表示を確認
2. 成功した場合 → OS32カーネル本体(`kernel/v86.c`, `isr_stub.asm`)に同じDS復元パターンを適用
3. 失敗した場合 → クラッシュダンプの`eip`から問題箇所を特定

---

## ファイル一覧

| ファイル | 説明 |
|----------|------|
| [v86_fdd_test.asm](file:///mnt/c/WATCOM/src/os32/tests/v86_fdd_test.asm) | FDDブート用V86テスト本体 |
| [boot_fat.asm](file:///mnt/c/WATCOM/src/os32/boot/boot_fat.asm) | 参照元: FDDブートローダー |
| [loader_fat.asm](file:///mnt/c/WATCOM/src/os32/boot/loader_fat.asm) | 参照元: GDT動的計算方式 |
| [mkd88.py](file:///mnt/c/WATCOM/src/os32/tools/mkd88.py) | RAW→D88変換ツール |
