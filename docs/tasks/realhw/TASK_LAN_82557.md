# TASK_LAN_82557 — 内蔵 LAN (Intel 82557、PCI) で Host Services を動かす

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-22) / 状態: **計画 (ユーザー決裁待ち §3)**。
> ユーザー指示 2026-09-22: 「シリアル転送が実用的な速度になったので、次は内蔵 LAN によるホストサービスの稼働を目指す」

正典: [`PLAN.md`](PLAN.md) §5 (82557 を狙う理由)・§6 (PCI の土台)、[`../v3/PLAN.md`](../v3/PLAN.md) §1 (順序) ・§3 (ドライバの動的読み込み)、
リンク層と Host Services は [`../network/LINK_PLAN.md`](../network/LINK_PLAN.md) / [`../network/HOST_SERVICES_PLAN.md`](../network/HOST_SERVICES_PLAN.md)。
資料: `docs/hw/undocumented/io_pci.md` (PC-98 の PCI)、**`docs/hw/intel/8255x_open_source_sdm.pdf`** (Intel 8255x Open Source Software Developer Manual、
341 ページ、2026-09-22 に Intel の公開 URL から取得。`docs/hw/` は著作権物のミラーで gitignore)。

## 0. 到達点

実機 PC-9821Ra266 のオンボード 82557 で、**既存のリンク層 (raw Ethernet、EtherType 0x88B5) と Host Services
(クリップボード・印刷・HTTP・時刻) がそのまま動く**こと。エミュレータ側の LGY-98 経路は残す (回帰用)。

## 1. 分かっていること

| 事実 | 出典 |
|---|---|
| NP21/W は PCI を再現しない (`0CF8h` の実装が無い)。**PCI も 82557 も実機でしか動かせない** | PLAN §5/§6 |
| 実機との会話はシリアル 115200 で 3.2 KB/s、`tools/rshell_serial.py` (2026-09-22) | TASK_SERIAL_VFAST |
| PC-98 の PCI: コンフィギュレーションメカニズム #1 (`0CF8h` DWORD でアドレス、`0CFCh` でデータ)。マーキュリー (82434LX) だけ #2 が要る。PCMC = デバイス 0、PCI-C バスブリッヂ = デバイス 1、スロット #0〜2 = デバイス 8〜10 | `io_pci.md` 40〜75 行 |
| PIRQ0〜3 → 8259 の割り当ては C バスブリッヂの PCI コンフィギュレーションレジスタで、通常 PnP BIOS が起動時に設定 → **デバイスの Interrupt Line レジスタを読めば IRQ が分かる** | 同 315〜320 行 |
| ハードは規格どおりの PCI (PC/AT 用ボードが挿さる) | 同 |
| リンク層は NIC の上に載る。`drivers/lgy98.c` が `link_init(mac)` を呼び、IRQ スタブは `irq_stub_nic_3/5/6` | `drivers/lgy98.c` 122〜137 行 |
| ホスト側 `tools/host_agent.py` は **TCP の FrameStream** (NP21/W の NP2NETSOCK が繋ぐ) しか話さない。実機では LAN の生フレームを FrameStream に橋渡しする道具が要る | `host_agent.py` `open_stream()` |
| WSL2 (ミラーモード) は `eth3` (物理、f0:68:e3:fa:99:06) を見るが AF_PACKET は root 要 (sudoers に python は無い)。Windows 側には **Npcap 導入済み** (`System32\Npcap`)、scapy は未導入 (uv で入る) | 2026-09-22 実測 |
| カーネル本体 442.6KB / 予算 468KB (**残り 25.4KB**)。予算は `MEM_KERNEL_IMAGE_MAX` = 帯 − KHEAP − KAPI − ガード − SHM。増やすなら SHM を 16KB 単位で削るか KHEAP を減らす | `docs/02_memory.md`、`include/memmap.h` 228 行 |
| v3 の順序は C11 → メモリマップ再配置 → **ドライバ動的読み込み → PCI → 82557** (「動かさない」)。理由は予算 (PCI 数 KB + 82557 十数 KB、USB は 100KB 超) | `../v3/PLAN.md` §1/§3 |

## 2. 段取り (案)

| 段 | やること | 大きさ (見積) | 検証 |
|---|---|---|---|
| **L-A** | **PCI の列挙**: `drivers/pci.c` (メカニズム #1 の config r/w、bus 0 の全デバイスを走査、vendor/device/class/BAR/Interrupt Line)、シェル `lspci`、`pci_find(vendor, device)`。判定は純粋関数に切り出してホスト試験 | 2〜3KB | **実機**: `lspci` で 8086:1229 (82557) と BAR (I/O 窓)、IRQ が出る |
| **L-B** | **82557 ドライバ (最小)**: PCI から I/O BAR と IRQ、SCB 経由で reset / EEPROM から MAC / Configure / IA setup / CU (TX: 1 コマンドずつ) / RU (RX: RFD 連結、簡略モード)、割り込みは RU の Frame Received + CU 完了。参照は Intel の SDM (§6 SCB、§7 RFD) と Linux e100 の作法 | 10〜15KB | **実機**: MAC が読める → 自己送信 (ループバック) → ホストへ HELLO |
| **L-C** | **NIC 境界の一般化**: `lgy98.c` ↔ `net/link.c` の結びつきを関数表 (`net/nic.h`: tx / mac / irq / poll) にして 82557 を 2 つ目の NIC に。起動時は PCI に 82557 が居ればそれ、無ければ LGY-98 (設定は手動でよい、v3 §3) | 1〜2KB | NP21/W (LGY-98 の回帰 L0〜L3) + 実機 |
| **L-D** | **ホストの橋** `tools/lan_bridge.py` (Windows 側 Python + scapy/Npcap): LAN アダプタで EtherType 0x88B5 を拾い、`host_agent.py --listen` の FrameStream へ流し、逆も返す | ホストのみ | 実機 ↔ ホスト |
| **L-E** | **Host Services の疎通**: 実機で `link_selftest` (L0〜L3 相当) → クリップボード / 印刷 / `wget` / TIME | — | 実機 |

L-A と L-D は独立 (並行できる)。L-B は L-A の上、L-C は L-B と同時、L-E は全部の上。

## 3. ユーザー決裁が要る点

1. **予算と v3 の順序**: L-A (2〜3KB) は残り 25KB に収まる。L-B (10〜15KB) を静的リンクすると予算をほぼ使い切る。
   v3 §1 は「ドライバ動的読み込み → PCI → 82557」の順を「動かさない」としている。選択肢:
   (a) 順序どおり: 先に v3 の 1〜3 (C11 / 再配置 / 動的読み込み) を片付ける (大きい、LAN は先送り)。
   (b) **PM の推奨**: L-A を先に (小さく、実機でしか検証できない土台を早く通す)、L-B は予算を測りながら静的で入れ、
       SHM を 16KB 削って余裕を作る (`kernel/shm.h` の `SHM_BLOCK_COUNT` と同時)。動的読み込みは 82557 が動いてから
       「外に出す最初のドライバ」として v3 §3 に戻す。
2. **ホスト側の橋の置き場**: Windows 側 (Npcap + scapy、venv に uv で導入) か、WSL2 側 (AF_PACKET、root が要る) か。
   **PM の推奨は Windows 側** (Npcap が既に入っている。sudoers を広げない)。
3. **配線**: 実機の LAN と Windows ホストが同じ L2 (同じスイッチ) に居ること。ルータ越しでは raw Ethernet は届かない。
4. **82557 で確かめてから** コンボカード (1394US2G-PCI) を判断する (PLAN §6 のまま)。

## 4. 実機でしか分からないこと (最初に測る)

- `lspci` の出力 (82557 のデバイス番号・BAR・Interrupt Line)。PC-98 の PIRQ→IRQ が何番に落ちているか。
- 82557 の I/O 窓が PC-98 の I/O 空間 (16 ビット) のどこに置かれているか (BIOS が割り当て済みか、0 なら自分で割り当てが要る)。
- バスマスタ DMA で読む RFD/CB の物理番地: OS32 のカーネル帯 (1MB 超) を 82557 は 32 ビットでアクセスできるので
  `0439h` の 1MB 制限 (FDC の DMA) は関係ない — ただし **ページング下で物理番地を渡す** (カーネル帯は恒等写像)。
