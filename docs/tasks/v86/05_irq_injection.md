# 5. 割り込み仮想化・IRQ注入メカニズム

ソース: [`v86.c`](../../../kernel/v86.c), [`v86_pic.c`](../../../kernel/v86_pic.c),
[`v86_vsync.c`](../../../kernel/v86_vsync.c), [`v86_session.c`](../../../kernel/v86_session.c)

## 5.1 IRQ処理の全体像

```
     ┌───────────────────────────────────────────────────────┐
     │  実ハードウェア IRQ                                   │
     │  ┌─────────┐ ┌─────────┐                              │
     │  │ IRQ0    │ │ IRQ1    │                              │
     │  │ (100Hz) │ │ (KBD)  │                              │
     │  └────┬────┘ └────┬────┘                              │
     │       │           │                                   │
     │       ▼           ▼                                   │
     │  isr_stub_20   isr_stub_21                            │
     │  (timer)       (keyboard)                             │
     │       │           │                                   │
     │       ▼           ▼                                   │
     │  timer_handler  kbd_handler                            │
     │       │           │                                   │
     │  ┌────┴────┐  ┌───┴───────────┐                       │
     │  │ V86?    │  │ V86?          │                       │
     │  │ Yes     │  │ Yes           │                       │
     │  ▼         │  ▼               │                       │
     │  v86_inject│  v86_kbd_enqueue │                       │
     │  _timer_irq│  → kbd_buf追加  │                       │
     │  (HW直接  │  → pending_irq  │                       │
     │   注入)    │    bit1セット    │                       │
     └───────┬───┘  └────────┬──────┘                       │
             │               │                               │
             ▼               ▼                               │
     ┌──── GPハンドラ末尾 (v86_pending_irq チェック) ────────┐│
     │  仮想IF=1 && pending_irq ≠ 0 → IVTから注入          ││
     │  仮想IF=0 → 保留のまま                               ││
     └──────────────────────────────────────────────────────┘│
                                                              │
     ┌───────────────────────────────────────────────────────┘
     │  ※ VSYNC IRQ2 は timer_handler 経由で 50Hz 間引き注入
     └─────────────────────────────────────────────────────────
```

## 5.2 IRQ0 (タイマ) 注入 — HW直接注入方式

### 注入タイミング

OS32のtimer_handler (100Hz) がV86モード中に呼ばれた場合、
割り込みリターン前にV86スタックにフレームを構築して
ゲストのINT 08hハンドラにリダイレクトする。

```
timer_handler (isr_stub.asm → irq_timer.c)
  │
  ├── V86モード中? (EFLAGS.VM チェック)
  │    No → 通常処理
  │    Yes ↓
  │
  ├── PIT分周比チェック (v86_pit_get_irq_divisor)
  │    divisor=2 なら 2tick に1回のみ注入 (50Hz)
  │
  ├── 仮想IF=1?
  │    No → v86_pending_irq |= (1<<0) で保留
  │
  ├── 仮想ISRでIRQ0処理中?
  │    Yes → 保留 (ISRスタック防止)
  │
  ├── IVT[0x08] がダミーIRET?
  │    Yes → スキップ (ハンドラ未設定)
  │
  └── ★ HW直接注入 ★
       V86のスタックに FLAGS/CS/IP を push
       CS:IP を IVT[0x08] に書き換え
       仮想ISR bit0 セット
       仮想IF=0
       IRETD → ゲストのINT 08hハンドラに遷移
```

### HW直接注入の仕組み

`v86_inject_timer_irq()` はtimer_handler内で呼ばれ、
CPUの自動pushされたV86スタックフレーム (regs[]) を直接書き換える:

```c
/* V86スタックに割り込みフレームを push */
regs[HWIRQ_REG_ESP] -= 2;
*(u16 *)v86_phys_addr(SS, ESP) = FLAGS;   /* EFLAGS 保存 */
regs[HWIRQ_REG_ESP] -= 2;
*(u16 *)v86_phys_addr(SS, ESP) = CS;      /* CS 保存 */
regs[HWIRQ_REG_ESP] -= 2;
*(u16 *)v86_phys_addr(SS, ESP) = IP;      /* IP 保存 */

/* 復帰先をIVTハンドラに書き換え */
regs[HWIRQ_REG_CS]  = handler_seg;
regs[HWIRQ_REG_EIP] = handler_off;
```

### PIT分周比の影響

ゲストがCounter#0を変更した場合、`v86_pit_get_irq_divisor()` で分周比を計算:

| ゲストのreload値 | OS32デフォルト | 分周比 | 実効レート |
|-----------------|--------------|--------|-----------|
| 0x4E00 (19968) | 0x4E00 | 1 | 100Hz (毎tick) |
| 0x9C00 (39936) | 0x4E00 | 2 | 50Hz (2tick毎) |
| 0x0100 (256) | 0x4E00 | 1 (最小) | 100Hz (上限) |

## 5.3 IRQ1 (キーボード) 注入 — GP保留注入方式

### キーボードデータの流れ

```
[実ハードウェア IRQ1]
  │
  ▼
kbd_handler (kbd.c)
  ├── スキャンコード読み取り (port 0x41)
  ├── 通常処理 (key_pressed[] 更新、shift状態更新)
  └── v86_active? → v86_kbd_enqueue(scancode)
       │
       ├── v86_kbd_buf[] にスキャンコード追加
       └── v86_pending_irq |= (1 << 1)  /* IRQ1保留 */

[GPハンドラ末尾]
  │
  ├── v86_virtual_if == 1?
  ├── v86_pending_irq & (1<<1)?
  └── Yes → IVT[0x09] のハンドラに転送 (GP保留注入)
```

### キーボードバッファ

```c
static u8 v86_kbd_buf[V86_KBD_BUF_SIZE]; /* 32バイトリングバッファ */
static volatile int v86_kbd_head, v86_kbd_tail;
```

ゲストがIN 0x41を実行すると、GPハンドラ内で`v86_kbd_buf`からデキュー。
IN 0x43 (ステータス) ではバッファにデータがあれば bit1 (RxRDY) をセット。

## 5.4 IRQ2 (VSYNC) 注入

### VSYNC注入の2つの方式

1. **ワンショット方式**: ゲストがOUT 0x64で「アーミング」→ 次のVSYNCタイミングで1回だけINT 0Ah注入
2. **フリーラン方式 (未実装)**: ネイティブモードで常時注入 — ゲストIVTにIRETのみの場合ハングするため無効

### 注入レート

```
OS32タイマ = 100Hz
実機VSYNC = 56.4Hz (400ラインモード)
VSYNC_INJECT_PERIOD = 2  → 100Hz / 2 = 50Hz (実機に近い)
```

## 5.5 仮想PIC (v86_pic.c)

### 仮想レジスタ

```c
static struct {
    u8 imr;        /* 割り込みマスクレジスタ */
    u8 isr;        /* In-Service レジスタ */
    u8 irr;        /* Interrupt Request レジスタ */
    u8 icw_step;   /* ICW受信フェーズ (0=通常, 1-4=ICW1-4) */
    u8 icw1;       /* ICW1 保存 */
    u8 icw2_base;  /* ICW2 ベクタベース */
    u8 read_isr;   /* OCW3: 1=ISR読み出し, 0=IRR読み出し */
    u8 flags;      /* フラグ: 初期化済み等 */
} vpic[2];         /* [0]=マスタ, [1]=スレーブ */
```

### EOI処理

ゲストがEOI (OUT 0x00, 0x20) を発行すると、仮想ISRの最高優先ビットをクリア:

```c
case 0x20: /* 非特定EOI */
    /* ISRの最上位ビットをクリア */
    for (bit = 0; bit < 8; bit++) {
        if (vpic[which].isr & (1 << bit)) {
            vpic[which].isr &= ~(1 << bit);
            break;
        }
    }
```

### ISR自動クリア (安全装置)

ゲストがEOIを発行せずにループした場合の保護:

- **100tick無EOI自動クリア**: ISRビットが100tick (1秒) 以上立ち続けた場合、自動クリア
- **v86_pending_irq との連動**: pending_irq のビットがある限り、GPハンドラは
  仮想IF=1 のタイミングでIVT転送を試行し続ける

## 5.6 保留IRQ注入 (GPハンドラ末尾)

```c
/* GPハンドラ末尾 (全命令エミュレーション後) */
if (v86_virtual_if && v86_pending_irq) {
    int irq;
    for (irq = 0; irq < 8; irq++) {
        if (v86_pending_irq & (1 << irq)) {
            /* IMRでマスクされていないか確認 */
            if (vpic[0].imr & (1 << irq)) continue;
            /* ISRで処理中でないか確認 */
            if (vpic[0].isr & (1 << irq)) continue;
            /* IVTから注入 */
            inject_sw_int(regs, base_vector + irq);
            v86_pending_irq &= ~(1 << irq);
            break;  /* 1回に1つだけ */
        }
    }
}
```

## 5.7 デバッグカウンタ

| 変数名 | 意味 |
|--------|------|
| `v86_irq0_inject_count` | IRQ0がゲストに注入された回数 |
| `v86_irq0_call_count` | timer_handler内のV86パスが呼ばれた回数 |
| `v86_irq0_nonvm_count` | V86モードでないためスキップした回数 |
| `v86_irq0_noif_count` | 仮想IF=0でスキップした回数 |
| `v86_irq0_isr_count` | ISR処理中でスキップした回数 |
| `v86_irq0_ivt_count` | IVTがダミーのためスキップした回数 |
| `v86_vsync_arm_count` | VSYNC arming回数 |
| `v86_vsync_inject_count` | VSYNC注入回数 |
