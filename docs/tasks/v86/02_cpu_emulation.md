# 2. CPUエミュレーション — #GPハンドラ詳細

ソース: [`v86.c`](../../../kernel/v86.c)

## 2.1 処理フロー

```
#GP 例外 (例外13)
  │
  ▼
isr_stub.asm: isr_stub_13
  ├── PUSHAD (汎用レジスタ保存)
  ├── V86フラグ確認 (EFLAGS.VM)
  └── call v86_gp_handler(regs)
       │
       ├── 1. IRQ受信窓の開放 (STI → CLI)
       ├── 2. タイムアウトチェック
       ├── 3. BIOS ROM領域チェック (CS >= 0xF000)
       ├── 4. プレフィックスループ (0x66/0x67/セグメント/LOCK/FWAIT/REP)
       ├── 5. オペコード別処理 (switch)
       ├── 6. 保留IRQ注入 (v86_virtual_if && v86_pending_irq)
       └── return 0 (続行) / 1 (終了)
```

## 2.2 レジスタ配列 (regs[])

`isr_stub.asm` のPUSHAD + CPUの自動pushにより、以下の配列が構成される:

```
regs[0]  = EDI        (PUSHAD)
regs[1]  = ESI        (PUSHAD)
regs[2]  = EBP        (PUSHAD)
regs[3]  = ESP_dummy  (PUSHAD — 使用しない)
regs[4]  = EBX        (PUSHAD)
regs[5]  = EDX        (PUSHAD)
regs[6]  = ECX        (PUSHAD)
regs[7]  = EAX        (PUSHAD)
regs[8]  = error_code (CPU auto-push)
regs[9]  = EIP        (CPU auto-push — フォルト時の命令位置)
regs[10] = CS         (CPU auto-push)
regs[11] = EFLAGS     (CPU auto-push)
regs[12] = ESP        (CPU auto-push — V86スタックポインタ)
regs[13] = SS         (CPU auto-push — V86スタックセグメント)
regs[14] = ES         (CPU auto-push — V86モード時のみ)
regs[15] = DS         (CPU auto-push)
regs[16] = FS         (CPU auto-push)
regs[17] = GS         (CPU auto-push)
```

## 2.3 命令デコード詳細

### プレフィックス処理

プレフィックスバイトをループで読み飛ばし、フラグを記録:

| プレフィックス | 動作 |
|--------------|------|
| 0x66 | `is_opsz32 = 1` — PUSHFD/POPFD と 32bit I/O に影響 |
| 0x67 | アドレスサイズ — 読み飛ばし |
| 0x26/0x2E/0x36/0x3E/0x64/0x65 | セグメントオーバーライド — 読み飛ばし |
| 0xF0 | LOCK — 読み飛ばし (メモリ操作は#GPしない) |
| 0x9B | FWAIT — NOP扱い |
| 0xF3 | `is_rep = 1` — INSB/OUTSB のREPに使用 |
| 0xF2 | `is_repne = 1` — 現在未使用 |

### INT n (0xCD)

1. BIOSインターセプト: INT 18h → `v86_bios_int18()`, INT 1Bh → `v86_bios_int1b()`, etc.
2. DOS終了検知: INT 20h / INT 21h AH=4Ch → V86終了 (非ネイティブモード)
3. ダミーIVT検出: ハンドラが 0x003F:0x0000 (IRET) → CF=1, AH=0x86 で即復帰
4. IVT転送: V86スタックに FLAGS/CS/IP をpush → IVTハンドラにジャンプ

### I/O命令のディスパッチチェーン

```
v86_in8_checked(port):
  保護ポート? → 0xFF返却
  キーボード (0x41/0x43)? → v86_kbd_buf から返却
  v86_pic_io()  → PIC仮想化
  v86_pit_io()  → PIT仮想化
  v86_fdc_io()  → FDC仮想化
  v86_dma_io()  → DMA仮想化
  v86_vsync_io() → VSYNC仮想化
  いずれも非対象 → inp(port) 実ハードウェア読み取り
```

### EIP加算の計算

```
新EIP = (旧EIP + prefix_len + 命令長) & 0xFFFF
```

- INT n: +2 (0xCD + nn)
- CLI/STI/PUSHF/POPF/IRET/HLT: +1
- IN/OUT imm8: +2
- IN/OUT DX: +1
- INSB/OUTSB: +1

## 2.4 仮想IFフラグの管理

実EFLAGSのIFビットはOS32カーネルが管理する。ゲストのCLI/STIは
`v86_virtual_if` (グローバル変数) を操作するだけで、実際のIFは変更しない。

```
CLI  → v86_virtual_if = 0        (仮想IF=0)
STI  → v86_virtual_if = EFLAGS_IF (仮想IF=1)
PUSHF → push(EFLAGS | v86_virtual_if)
POPF  → v86_virtual_if = (popped_flags & EFLAGS_IF)
INT n → v86_virtual_if = 0  (割り込みはIF=0にする)
IRET  → v86_virtual_if = (popped_flags & EFLAGS_IF)
```

## 2.5 タイムアウト機構

2つのタイムアウト検出ポイントがある:

1. **GPハンドラ内** (`v86.c`): 特権命令実行ごとにチェック → `return 1` で即V86終了
2. **`v86_inject_timer_irq` 内** (`v86.c`): ゲストが通常命令のみのループに入った場合、
   GPが発生しない。100Hz IRQ0は必ず発火するため、ここでHLT注入方式でタイムアウトを検出する。

タイムアウト時の処理 (HLT注入方式 — `v86_inject_timer_irq` 内):

1. 実行中のゲスト CS:IP を記録 (`v86_timeout_cs`, `v86_timeout_ip`)
2. ゲストの CS:EIP を HLT 命令配置先 (`0x003C:0x0009`) に強制書き換え
3. `v86_exit_request = 1` をセット
4. 次の IRETD で V86 に戻ると HLT が実行され、#GP → HLT ケースで `return 1`

> **注記**: 以前 `timer_handler` 内に存在した直接 `exec_longjmp` 方式のタイムアウトは
> スタック破壊リスクのため廃止済み。HLT注入方式に一本化されている。

## 2.6 トレースリングバッファ

最近128件のGPイベントを記録:

```c
struct v86_trace_entry {
    u16 cs, ip;       /* フォルト位置 */
    u8  opcode;       /* プライマリオペコード */
    u8  intno;        /* INT n の場合のn */
    u8  ah, al;       /* INT n の場合のAX */
    u16 cx;           /* ECX下位16bit */
};
```

## 2.7 未実装命令とDOSへの影響

### INSD/OUTSD (0x6C+0x66 / 0x6E+0x66)

32bit文字列I/O。DOSやPC-98ネイティブソフトでの使用例はほぼないが、
386対応ドライバでは使用される可能性がある。

### 0x0F 2バイトオペコード (LGDT/LIDT/LMSW/MOV CRn等)

DOSエクステンダ (DOS/4G, DPMI等) がプロテクトモードに遷移する際に使用。
MS-DOS本体の起動には不要だが、DOS上のプロテクトモードアプリ実行には必要。

> **判断**: MS-DOS起動が最優先目標であり、DOSエクステンダ対応は後回し。
