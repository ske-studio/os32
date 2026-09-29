# TASK_MEMMAP_V3 — v3 のメモリマップ (カーネル帯の切り直し・アプリ帯の仮想化・物理台帳)

> 状態: **設計中 (2026-09-30)** — **方針確定**。2026-09-30 にユーザー・Fable 5.1・Codex (gpt-6-astra) の 3 者討論で決定 (Codex 最終確認 **Approve**、ユーザーの判断もすべて確定)。実装は §6 の票 T0〜T7 の順で、未着手。**§3-5 (池の運用規則) は Codex 往復 6 の突き合わせを 2026-09-30 に反映 (§8-3)。R2 (実行中の伸長) は推奨案でユーザー確認待ち。**
> それまでの状態: 設計 v2 (Codex 往復 1 の 9 件を反映。2-2 は保留、2-3 (KHEAP 192KB) は着地) — 経緯は §11。
>
> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-23)。出所: ユーザー指示 2026-09-23「カーネル予算はシュリンクではなく考え直す。順に実行」→ 2026-09-29「3 者で討論して決める」。
> 正典の関係: 番地の正典は `include/memmap.h`、地図は [`../../02_memory.md`](../../02_memory.md) §2-1 (生成)、v3 全体の柱と順序は [`V3_PLAN_DRAFT.md`](V3_PLAN_DRAFT.md) §3、目標の 2 段は同 §2。
> 討論前の経緯は [`../../archive/kernel_v21/TASK_KSTACK_USER.md`](../../archive/kernel_v21/TASK_KSTACK_USER.md)。
> 討論の基点は `feat/gui` 39a09b89 (KernelAPI v68)。本文の `file:line` はその基点の行で、数字は `docs/02_memory.md` §2-1 の生成ブロックと `size` / `ls -l` / `readelf` の実測 (容量は KiB)。**「推測」と書いたものは未確認**で §10 に集める。

---

## 0. 決定事項の一覧 (D1〜D22)

| # | 決定 | 出所 |
|---|---|---|
| D1 | **カーネル・シェル・通常 RAM は恒等写像 (supervisor)**、**アプリだけ 0x80000000〜 の私有写像** | 両者合意、ユーザー承認 |
| D2 | **物理地図と所有権台帳を先に** (RAM の存在 / 割当可否 / owner / 写像を別情報に、PFN 半開区間) | Codex P0、両者合意 |
| D3 | `exec_child_claim` / `sys_reserve_top` / `EXEC_DYN_RESERVE` / `MEM_APP_BAND_DEVICE_FLOOR` は撤去。物理は全部池から owner 付き | 両者合意 |
| D4 | 物理池の境界定数とアプリ仮想配置定数を分離 | 両者合意 |
| D5 | DMA プールは 64KB 整列、ISA の 16MB 未満・境界は割当条件 (`dma_alloc(size, align, limit)`) | 両者合意 |
| D6 | 15〜16MB は既定で予約 | 両者合意 |
| D7 | 全再ビルド・旧形式は拒否・KAPI スロット順は維持。互換層なし。**KAPI / ABI の後方互換は基本考えない** (ソース互換は極力維持するが絶対ではない) | ユーザー (2026-09-29、09-30) |
| D8 | シェルの 2 ヒープは API 2・供給元 1 | 両者合意 |
| D9 | 8MB 機は **9801 planar と PEGC の両方で GUI + 私有メモリ総量 2MB のアプリ** (内訳は任意) | ユーザー |
| D10 | 通常アプリの `--cpl0` 廃止。バックエンド・ドライバは CPL=0 モジュール (当面は静的)、常駐シェルは例外 | ユーザー (当面許可) |
| D11 | アプリ帯の起点 0x80000000 (RAM の登録上限 2GB) | ユーザー |
| D12 | SQLite はカーネルの機能として残すが同一リンクにしない | ユーザー |
| D13 | 低位 640KB は V86 用などに解放してよい | ユーザー |
| D14 | フォントは OpenType へ。**`.kcgfont` (IPAex から焼いたビットマップ) は廃止**。**漢字 ROM (KCG ROM) は予備として残す** (モジュール 4KB) | ユーザー (2026-09-30) |
| D15 | 同時使用はしない。快適の判定は前景 1 本 (起動 2 秒・入力→描画 100ms・マルチメディア 1 本、比較は P100/32MB の Win95) | ユーザー |
| D16 | 進め方: Fable 最終案 → Codex 突き合わせ → この票と `V3_PLAN_DRAFT` に記録 | ユーザー |
| D17 | コア / モジュールの区分 (§4-4)。**FDC はコア** (最終ブート手段) | ユーザー (2026-09-30) |
| D18 | ブートスプラッシュはユーザーランドで実行してからアンロード。表示は**シリアルのやり取り (rshell / SerialFS) の前で十分** = ルート FS をマウントした後の最初のアプリ | ユーザー (2026-09-30) |
| D19 | **BB は v3 の最初は gshell 所有 + 全画面 lease** ((b)、§5-7)。**PEGC 直描きは全画面 lease に限る**。アプリ私有サーフェス + WM 合成 ((c)) は**サーフェス層の票**で。**P8 (音・入力) / P9 (Video HAL) は別の柱** | ユーザー (2026-09-30) |
| D20 | **カーネルの仮想アドレス化 (高位カーネル) は v4 以降の候補。v3 は恒等写像のまま、物理⇔仮想の変換を P2V / V2P の 1 か所に集める下準備だけ行う** (§3-4、票は T1) | ユーザー (2026-09-30) |
| D21 | **SQLite と FEP は常に読み込む。例外は FD 起動の回復用構成 (MINIMAL) だけ** (その場合 `db_*` は「機能なし」の決まった誤りを返す)。FEP の任意省略は決めない。理由: SQLite は KAPI 経由でユーザーランドに開放された機能 (`settings.db`、libos32cfg、libos32db、install)。8MB の勘定は読み込んだ構成でも +1.57〜1.74MB 余る (§4-3) | ユーザー (2026-09-30 確定) |
| D22 | 目標の 2 段: **最低条件** = 8MB 機で GUI + 私有 2MB のアプリ (9801 / PEGC)、**快適さの判定** = D15、**目標** = 32〜64MB 機で前景 1 本について Windows 2000 当時のアプリの使用感。正典は [`V3_PLAN_DRAFT.md`](V3_PLAN_DRAFT.md) §2 | ユーザー (2026-09-30 確定) |

---

## 1. 目的 — 何を解決するか

討論前の地図 (v2.1、§11-1) の問題は「1MB のカーネル帯に 1,041KB を詰めた」(TASK_KSTACK_USER §7-4) の再来を避けることだが、根はもう 1 段深い:

1. **PDE 0 (先頭 4MB) が満杯**で、シェル・shlib・アプリ帯・SQLite が互いの番地に縛られている (番地を付け直すだけの案 A では PDE 1 にシェルが落ちて CPL=3 起動で写像が消える、§11-2 B1)。
2. **アプリの仮想帯が物理の地図に縛られている** (デバイス窓・静的 PT・`exec_child_claim` の範囲・PEGC 窓 0xF00000 の天井)。32MB の機で 1 本のアプリに渡せる量が 3MB 級に留まる。
3. **物理の所有者が台帳に無い** (`sys_reserve_top` の引き算、`EXEC_DYN_RESERVE` の穴、V86 の 636KB バッキング、BB の全 AS 共有 USER 写像) ので、8MB 機の成立を計算で示せない。
4. 低位 640KB がフォントキャッシュ (292KB)・Unicode 表 (128KB)・BB (128KB) に使われ、V86 に渡す領域と時分割。
5. SQLite (770KB) が本体と同一リンクで固定帯 0x200000 を占め、伸び代 44KB。

v3 の答え: **システム側は恒等のまま (D1・D20)、物理地図と所有権台帳を先に作り (D2)、アプリ帯だけを RAM の上 0x80000000 へ出す (D11)。** 固定帯は 3MB のカーネル帯 + 1MB のシェル帯だけにし、残りは台帳が owner 付きで配る「池」にする。SQLite・FEP・音源・LAN・NP21/W 専用はモジュールとして池へ (D12・D17)、低位は V86 に返す (D13)。

---

## 2. 最終のメモリマップ

### 2-1. 帯の表

「固定」= 起動時に決まり台帳では owner=kernel。「池」= 台帳が owner 付きで配る恒等ページ。**通常アプリ (CPL=3 の AS) の不変条件: PDE 0〜511 に USER の PTE は、トランポリン (RO) と SHM (契約で全アプリ共有) 以外に存在しない**。**V86 セッションは例外** (§2-2 の V86 の項): セッション中だけ master の低位 (ページ 0・アリーナ・VRAM・ROM) に USER を立て、終了時に復元する既存機構 (`v86_mem.c:73,99`) を保つ。

| 番地 (物理 = 仮想。アプリ帯と lease 窓だけ仮想) | PDE | 種別 | 大きさ | 中身 |
|---|---|---|---|---|
| 0x000000–0x000FFF | 0 | 固定 NP | 4KB | NULL ガード |
| **0x001000–0x09FFFF** | 0 | **V86 アリーナ** (D13) | 636KB | 起動時だけ bootinfo 0x7E00。フォント・Unicode・BB は出て行く (§5)。0x90000 の自動プレイ mailbox は残す |
| 0x0A0000–0x0EFFFF | 0 | 固定 **supervisor** | 320KB | VRAM (テキスト + 4 プレーン + PEGC のバンク窓)。**アプリには常時 USER で見せない** — CUI の TVRAM 直書き・全画面は lease 窓 (下) 経由。**V86 は低位そのもの** (ゲストの `A800:0000` は 0xA8000) なのでセッション中だけ低位に USER を立てる (§2-2) |
| 0x0F0000–0x0FFFFF | 0 | 固定 RO | 64KB | BIOS ROM |
| **0x100000–(鎖の終端)** | 0 | 固定 (浮動の鎖) | コアだけなら ≈ 826KB (推測) | 本体 (§4-4 のコア ≈ 370KB + Unicode 組表 28KB、推測) → KHEAP 192KB → KAPI 4KB → ガード → SHM 224KB (末尾 64KB GUI) → ガード。**リンカ ASSERT: 鎖の終端 ≤ 0x3E0000** (本体の予算 2,516KB)。トランポリンページ (RO+USER) と SHM (RW+USER) だけが USER |
| **(鎖の終端)–0x3DFFFF** | 0 | **池 (KERNEL_SLACK)** | 今の本体 583KB なら 1,972KB (Codex 算定)、コアだけなら ≈ 2,118KB (推測: 2,146 − Unicode 28) | 未使用ページをブート時に台帳へ RAM (source=KERNEL_SLACK) で登録。present・supervisor なので張り直し不要。モジュールの第一候補 |
| 0x3E0000–0x3EFFFF | 0 | 固定 | 64KB | DMA プール (64KB 整列、D5) |
| 0x3F0000–0x3F0FFF | 0 | NP | 4KB | ガード |
| 0x3F1000–0x3FAFFF | 0 | 固定 | 40KB | 固定ページ表 (PD 4 + ブート PT 32 + デバイス窓 PT 4、`paging.c:155,174`) — 画像の外 (旧 2-1) |
| 0x3FB000–0x3FBFFF | 0 | NP | 4KB | ガード |
| 0x3FC000–0x3FFFFF | 0 | 固定 | 16KB | カーネルスタック |
| **0x400000–0x4FFFFF** | 1 | 固定 supervisor | 1MB | **常駐シェル帯 (1 箱)**: 画像 (gshell 219KB) → sbrk ↑ … exec_heap ↓ ← ガード ← スタック 40KB。全 PD 共有。gshell は CPL=0 なので池・VRAM 窓を直接触れる (D8) |
| **0x500000–0xEFFFFF** | 1–3 | **池 (低位側)** | 10MB | `MEM_POOL_BASE = 0x500000`。ブート時は **0x500000–0x5FFFFF = 圧縮画像の集積域、0x600000–0x6FFFFF = 同梱モジュール域** (§4-5、台帳初期化で予約) |
| 0xF00000–0xFFFFFF | 3 | 固定 NP / sup+PCD | 1MB | システム空間 (PEGC リニア窓)。RAM として配らない (D6) |
| 0x1000000–RAM 上端 | 4– | 池 (高位) | 検出量 | K6 の経路。`MEM_PHYS_RAM_CEILING = 0x80000000` |
| 0x20000000 帯 | 128– | 固定 sup+PCD | BAR ごと | PCI の BAR (Ra266)。PT は列挙時 (`exec_init` より前) に作る |
| **0x80000000–0xEFFFFFFF** | **512–959** | **アプリ帯 (仮想、PD ごと私有)** | 4MB × `MEM_APP_BAND_MAX_PDES` (提案 64) | shlib 0x80000000〜 (仮想 1MB、物理は実ページ数) → exec 0x80100000 → sbrk → ガード → exec_heap → スタック (**既定 256KB、OS32X ヘッダで可変**)。master には PDE を持たない |
| **0xF0000000–0xFDFFFFFF** | **960–1015** | **lease 窓 (仮想、PD ごと私有)** | 4MB 単位、要る PT だけ | **サーフェスの別名写像**: 主記憶サーフェス (BB、OpenType の共有キャッシュ等) と VRAM (全画面 / CUI の TVRAM) を、**その AS の私有 PT に** USER で張る (V86 は対象外、低位を直接使う)。低位の恒等は supervisor のまま。同じ物理を 2 つの AS に貸せば 2 つの別名ができる |
| 0xFE000000–0xFEFFFFFF | 1016–1019 | 固定 sup+PCD | 16MB | デバイス窓の帯 (v2.1 のまま) |
| 0xFF000000– | 1020–1023 | 固定 | 16MB | ROM ミラー / MMIO |

### 2-2. lease の契約

- **lease = 台帳の SURFACE (owner、物理範囲または VRAM 範囲、幅・高さ・ピッチ・形式) を 1 つの AS の lease 窓に USER (RW/RO、PCD は物理の属性を継承) で張ること。** `addrspace_map_user_page` の「共有 PT を書き換えて PDE に USER を立てる」経路 (`paging.c:796-812`) は**廃止**し、lease 窓の私有 PT にだけ書く。
- 解放は「その AS の lease を外す」だけで他 AS に波及しない。物理の解放は全 lease が返り、かつ owner が返したとき。
- 表示面 (planar の VRAM、PEGC のリニア窓 / バンク窓、Cirrus の表示面) は**全画面 lease だけ**に貸す (G4、D19)。通常 GUI アプリには VRAM の lease を与えない (`exec.c:1932` の常時 USER 写像を撤去)。CUI アプリの TVRAM 直書き (`lconsole` 等、U20) は TVRAM (0xA0000–0xA3FFF) だけの lease。
- **V86**: V86 ゲストは実アドレス (seg<<4)+off で低位を触るので lease 窓では届かない。**V86 セッション中だけ** `v86_mem.c` の既存機構どおり master の低位 (ページ 0 は実物、アリーナ 0x1000〜0x9FFFF、VRAM 0xA0000〜0xBFFFF、ROM) に USER を立て、**終了時に supervisor へ復元する** (`v86_mem.c:73,99`、`v86.c:93`)。V86 は CUI 専用 (`exec.c:1743-1747`) で通常アプリの AS とは同時に存在しないので、§2-1 の不変条件は「V86 セッション外」で検査する。
- BB の連続性: **ブート時 (gfx probe の直後、live AS = 0) に台帳から連続確保し、owner=boot → GUI 起動時に gshell へ移譲、GUI 終了時も返さず保持** (再確保の失敗を避ける)。`rep movs` に要るのは仮想の連続だが、gshell は CPL=0・恒等で使うので**物理連続のまま持つ**のが最も単純。スプラッシュには同じ面を lease し、終了で lease だけ返る。
- 検査 (§2-3 の ⑥ (b)(c)) は「共有種別なら可」ではなく **「その AS の台帳に lease があるページなら可」**。

### 2-3. Codex 往復 2 の指摘への答え (設計に取り込んだもの)

| 指摘 | 答え |
|---|---|
| ① 3MB を永久占有しない | KERNEL_SLACK を池へ (§2-1) |
| ② 15〜16MB の穴 | 17MB → 11MB + SLACK、32MB → 26 + SLACK、64MB → 58 + SLACK |
| ③ shlib 184KB | 池から実ページ数、owner=shlib。text は各 AS の私有 PT に RO+USER (`shlib.c:262` の attach を AS ごとに)、data 複製は owner=そのアプリ (**私有総量 2MB の内側**) |
| ④ BB / 共有 USER / 後付け PDE | §2-2。共有 PDE の追加は最初の AS の前に全部 (池の PT、PCI BAR、デバイス窓)。lease 窓の PT は AS ごとの私有なので後付けの問題は無い |
| ⑤ ローダ | §4-5 に区間表と寿命 |
| ⑥ メモリマップ検査 | 3 段: **(a) master**: PDE 0〜池の上端と高位窓を台帳から期待値を引いて走査。USER はトランポリンと SHM だけ、デバイス窓・PCI BAR・**台帳に MMIO として登録した範囲 (PEGC のリニア窓 0xF00000〜0xF4AFFF を含む) は sup+PCD**、**15〜16MB のうち登録の無い部分は NP** (「RAM として配らない」と「NP」は別)。V86 セッション中は (a)(b) を走らせない。**(b) AS 生成時**: PDE 0〜511 が master と同一 (= USER の PTE はトランポリン・SHM だけ)。PDE 512〜959 の PTE は台帳で owner=その AS のページ (shlib text は owner=shlib を RO で許可)。PDE 960〜1015 の PTE は**その AS の lease に載っている物理だけ**。**(c) 毎起動**: (a) + 生成した試験 AS で (b) + lease を 2 つの AS に貸して互いに見えないこと |
| ⑦ 定数の分離 | 下の一覧 |
| ⑧ 既定 exec_heap の先取り | `heap_size == 0` は MEM_EXEC_HEAP_MIN から始め、失敗時にページ単位で伸ばす。sbrk も張ったぶんだけ。**伸ばす経路 (通常文脈の KAPI、大きな塊は map/unmap) は §3-5 R2** |

**定数の分離箇所 (⑦、D4)**

| 種別 | 新定数 | 触る箇所 |
|---|---|---|
| 物理 (池) | `MEM_POOL_BASE` 0x500000、`MEM_POOL_LOW_END` 0xF00000、`MEM_HIGH_RAM_BASE`、`MEM_PHYS_RAM_CEILING` 0x80000000、`MEM_BOOT_STAGING_*` / `MEM_BOOT_BUNDLE_*` (§4-5) | `physmem.c:97`、`paging.c:406` (`reserve_table` 撤去、model 経路に統一)、`pgalloc.c:109,120`、`memory_boot.c:145,208`、`shlib.c:95`、`memory_boot.c:12,15` (legacy 経路撤去) |
| 仮想 (アプリ) | `MEM_APP_BAND_BASE` 0x80000000、`MEM_SHLIB_BASE`、`MEM_EXEC_LOAD_ADDR` 0x80100000、`MEM_APP_BAND_MAX_PDES`、`MEM_LEASE_WINDOW_BASE` 0xF0000000、`RING3_*` | `paging.c:127-133` (ASSERT を `APP_BAND_PDE >= 512`、`LEASE_END ≤ DEVICE_APERTURE_BASE` に)、`:671`、`:696,735` (master PT 要件撤去)、`:796-812` (共有 PT 経路撤去)、`exec.c:1649`、`:1822`、`:1919`、`:1932-1959` (VRAM/SHM/フォント/Unicode/BB の USER 写像 → SHM とトランポリンだけ残す)、`ring3_ptr_ok` (`exec.c:310-324`)、`sdk/link/app.ld:8`、`app_sys.ld:5`、`shlib.ld:20,64`、`tools/mkshlib.py`、`stub.rs`、`build/os32.ld` |
| 撤去 (D3・D10) | — | `exec_child_claim` (`exec.c:939`)、`ring3_band_ram_top`、`exec_cpl0_claim/release` (`:975-1000`)、`sys_reserve_top` (`sys.c:185-215`)、`EXEC_DYN_RESERVE`、`MEM_APP_BAND_DEVICE_FLOOR` (`memmap.h:350`)、`--cpl0` (`mkos32x.py:16,98`)、`kernel.mk:201` の `--sqlite-addr`、`boot_splash.c` (アプリへ、D18) |

---

## 3. 物理台帳と P2V / V2P

### 3-1. 台帳 (D2)

RAM の存在 (物理地図)・割当可否・owner・写像を**別の情報**として持つ。単位は PFN の半開区間。種別: RAM (source = BOOT / KERNEL_SLACK / HIGH)、MMIO (PEGC 窓・PCI BAR・デバイス窓 — sup+PCD で張る範囲)、予約 (15〜16MB、集積域、同梱域)、SURFACE (§2-2)。owner タグ: kernel / boot / bundle / shlib / gshell / AS ごと / モジュール名。`exec_exit()` の owner 単位の回収 (CLAUDE.md の Architecture) と同じ考えを物理ページに広げる。

### 3-2. DMA (D5)

`dma_alloc(size, align, limit)` — 64KB 境界 ([HW2]) と ISA の 16MB 未満は**割当条件**であって固定帯の理由ではない。DMA プール 64KB (0x3E0000) は固定の塊の 1 つとして残す (PCM リング・82557 CB/RFD の顧客は既にある)。

### 3-3. 15〜16MB (D6)

既定で予約。セットアップで切り離した機 (資料 `io_mem.md`) でも RAM として配らない。地図検査は「登録の無い部分は NP、MMIO 登録 (PEGC リニア窓) は sup+PCD」で見る (§2-3 ⑥)。

### 3-4. P2V / V2P への集約 (D20 — 高位カーネルは v4 以降)

- **規則**: カーネルが物理番地をポインタとして触る箇所は `P2V(phys)` を通し、ポインタを装置 (DMA・ページ表・窓) に渡す箇所は `V2P(ptr)` を通す。恒等の v3 では両方とも**何もしない関数 (`static inline`) / マクロ** (`include/memmap.h` に 1 か所)。台帳の API は**物理 PFN / 物理番地を返す**ことにし、呼び手が P2V で触る形に揃える。
- **監査の対象 (Codex 往復 2 の一覧 + 追加)**: `kernel/paging.c` の PD/PT (`:717,737,794,836,889` の `(u32 *)phys`)、`kernel/shlib.c:208,285` の複製、`kernel/v86_mem.c:44` のバッキング、`gfx/backend_pegc.c:704-705` の BB、`drivers/fdc.c:787,816` / ATAPI / PCM の DMA バッファ (`dma_pool_alloc` の戻り値、V2P 側)、BIOS データ領域 (`backend_pegc.c:117-125` の 0000:0400h、`kbd_init` の BIOS ワーク)、VRAM 窓 (`PEGC_LINEAR_BASE`、`0xA8000` 系、Cirrus の `s_lin`)。`kmalloc` は対象外 (仮想のヒープ)。
- **票**: **T1 (台帳) の中**に置く — 台帳の API を作り替えるときに呼び手を全部触るので、同じ差分で P2V/V2P に揃えるのが最も安い。恒等なので動作は変わらず、危険は無い。**`tools/check_constraints.py` に検査を足す**: `kernel/ drivers/ gfx/ fs/ exec/` で `(u8 *)` / `(u32 *)` / `(void *)` を物理番地の変数・定数 (`*_phys`、`MEM_*_BASE`、`PEGC_LINEAR_BASE` など) に当てるキャストは P2V を通っていなければ違反 (ID は新設、例外は一覧で許可)。
- **v4 で高位化するとき**に変わるのはこの 1 か所と、lease 窓の別名 (既に非恒等) だけ、という状態を v3 の終わりに作る。Codex 往復 2 の W1〜W3 (live-AS 規則・exec の CR3 切替・監査対象の広さ) は v4 の設計で改めて解く。

---

### 3-5. 池の運用規則 (ユーザー決定 2026-09-30、Codex 往復 6 の突き合わせを反映 2026-09-30)

池 (§2-1) はカーネル側 (モジュール・boot・gshell) とアプリが同じ物理を分け合う。プリエンプティブでない (V3_PLAN_DRAFT P6 の意図: アプリは同時に起動して見えるだけ、前景が固まったらカーネルが取り戻す) ので、実行中の貸し借りは少ない前提で、**いつ・誰が・どう借りて返すか**を次の約束にする。R1〜R5 の「通常文脈」の定義と、P6 を見直すときに何が壊れるかは §3-5-3。**R2 は推奨案 (ユーザー確認待ち)**、それ以外は決定。

| 時点 | 誰が | 何を |
|---|---|---|
| 起動時 | カーネル | KERNEL_SLACK の登録、BB (owner=boot、連続) と同梱域の確保、DMA プールの予約 |
| モジュール読み込み | カーネル | SQLite (code・MEMSYS5・代替スタック)・FEP 等を owner=モジュール名で。大きさは読み込み時に確定。失敗は owner ごと巻き戻し (R7) |
| アプリの起動 | exec | 私有ページの**最小量** (code/BSS・スタック・ヘッダの heap_size 分)・PD/PT (lease 窓の PT 1 枚を含む)・shlib data の複製 |
| アプリの実行中 (syscall) | exec (通常文脈) | ヒープの伸長 (ページ単位) と大きな塊の map/unmap (R2)。失敗は NULL |
| アプリの終了 | exec (通常文脈) | owner=その AS のページを全部回収 (R5)。強制脱出でも回収はここ (R1) |
| GUI の開始と終了 | gshell | BB を boot から受け取る (永続 owner)。GUI を終えても返さず保持 |
| lease の付け外し | カーネル (通常文脈) | 物理は動かさず、lease 窓への写像だけ。PT が足りないときだけ池から 1 枚 (R3-e) |

#### 3-5-1. 規則 R1〜R7

- **R1 割り込みの中では池を確保・解放しない — ISR が行うのは要求と制御移譲だけ、回収は通常文脈で。**
  - ISR (IRQ ハンドラ本体、EOI まで) は台帳を触らない。触れば kselftest / assert で止める (これで池の排他はほぼ不要)。
  - **強制脱出 (CTRL+STOP・番犬・gfx 拒否) の契約**: IRQ が行うのは `abort_req` を立てること (要求) と、割り込まれた文脈が CPL=3 のときに**割り込みフレームを捨てて exec_run の復帰点へ制御を移すこと** (制御移譲) の 2 つだけ。AS の破棄・物理の返却・owner ごとの回収 (`exec_teardown_app`、`exec_reclaim_owned` → `pcm_reclaim` …) は、復帰点で IF=1 に戻してから **通常文脈** (§3-5-3) で行う。**これは P6 (前景が固まったらカーネルが取り戻す) の必須要件**で、番犬の形 (P6 の票) はこの契約の上に載る。
  - **現行との差 (Codex 往復 6 P1)**: 今は IRQ1 スタブ (`isr_stub.asm:527-538`、EOI と V86 反射の後、CS.RPL == 3 のときだけ) → `ring3_abort_check` (`exec.c:635`) → `exec_exit` → `exec_teardown_app` (`exec.c:1125`) → `exec_reclaim_owned` → `pcm_reclaim` → `kfree` (`pcm_cs4231.c:472`) と、**回収を割り込みフレームの中 (IF=0) で済ませてから** longjmp する。割り込まれた文脈が CPL=3 なのでカーネルの台帳は中途ではなく、だから今まで壊れなかった。だが IF=0 なので tick が止まり、回収の中で tick の期限を待つもの (`pcm_poll`、`pcm_cs4231.c:127-133`) は期限が来ない — 回収に「待ち」が 1 つでもあれば固まる。R1 の assert を入れれば CUI の無限ループ中の CTRL+STOP が assert で止まる (反例)。
  - **実装**: `ring3_abort_kill` を **(a) 移譲** (状態を ABORT_PENDING にし AS は残したまま longjmp) と **(b) 回収** (復帰点で `exec_exit(EXEC_KIND_ABORT)`) の 2 段に割る。syscall 入口の `ring3_abort_check` (`exec.c:1452`) と WM の OP_WAIT からの `exec_kill` は元から通常文脈なので (b) に直行。**票 T2** (exec / AS を触る票) で行う。**票 T1** は台帳に「割り込みの入れ子深さ > 0 で確保・解放されたら数える」検査を入れ (T1 の受入は件数を報告)、T2 で経路を割ってから panic に切り替える。受入: CUI の無限ループ中に CTRL+STOP → 池の空きが戻る、PCM 再生中の CTRL+STOP → `pcm_reclaim` がリングを止めて返す (tick が進むこと)。

- **R2 アプリの私有メモリ — 起動時は最小量、実行中の追加は通常文脈でカーネルに借りる。** (§2-3 ⑧ の側に揃える。**推奨案、ユーザー確認待ち**。出所: Codex 往復 6 P2「R2 と ⑧ の矛盾」+ ユーザーの問い 2026-09-30「エディタや起動後に開くメディアファイルの大きさ次第で乗り切らないときは再度借りるのか、返し方は」)
  1. **起動時に確保するのは最小量**: code/BSS・スタック (既定 256KB、OS32X ヘッダで可変)・ヘッダの `heap_size` 指定分 (未指定 = `MEM_EXEC_HEAP_MIN` 64KB、`memmap.h:434`)・shlib data の複製・PD/PT。これが取れなければ起動を拒否する。予算の決まったアプリ (ゲーム等) は `heap_size` で起動時に確保しきる — 旧 R2 の「起動時に確保しきる」はこの形として残る。
  2. **実行中の追加は通常文脈 (syscall) でカーネルに借りる**。2 種類: **(i) ヒープの伸長** — sbrk / exec_heap の上限をページ単位で上げる (物理は池から owner=その AS、仮想はアプリ帯の中で連続)。現行の `_sbrk` (`sdk/crt/syscalls.c:162`) は `kapi->sbrk_heap_limit` を見て断るだけ、`exec_heap_alloc` (`exec_heap.c:28`) も池を借りないので、上限を上げる KAPI 1 本 (`brk` 相当) を足す ([ABI2] 追記)。**(ii) 大きな塊** (メディアファイル・大きな文書・画像面) は**別の領域として借りる KAPI (`mem_map(size)` / `mem_unmap(ptr)`、mmap 的)** — アプリ帯の中 (exec_heap とスタックの間、または帯の上側) に仮想を予約し、物理を池から owner=その AS で張る。**失敗は NULL / −1 (ENOMEM)** で、スワップが無いのでアプリが扱う (縮小版で開く・断る)。**上限は機種の空き次第** — D9 の「私有 2MB」は 8MB 機の受入条件であって上限ではない (Codex 往復 6 P2)。仮想の上限はアプリ帯 `MEM_APP_BAND_MAX_PDES` × 4MB (提案 64 = 256MB)。
  3. **返し方**: (ii) の塊は `mem_unmap` した時点で**池へ即返す** (owner の台帳から外し、その AS の PTE を NP に、TLB を無効化)。(i) のヒープは**終了時にまとめて返す**のを既定にし、末尾が空いたら縮める (trim) かは**ユーザー判断** (小さい割り当ては終了時まで保持、大きい塊は即返却、が推奨)。アプリの終了時は owner ごと全部回収 (R5)。
  4. **起動時に確保済みのカーネル側資源 (BB・モジュール・DMA プール・KHEAP・シェル帯) はアプリの伸長で奪われない** — R3 / R4 で起動時・読み込み時に取ってあり、池の空き = 登録 − 固定 − 起動時確保 の残りだけがアプリに配られる。
  5. **8MB 機で伸長が池を使い切ったとき GUI・カーネルが巻き込まれない根拠**: gshell はシェル帯 (固定 1MB) で動き BB は確保済み、KHEAP は固定 192KB、モジュールは読み込み済みで実行中に池を借りない (R3) — 池が 0 になっても走っているものは止まらず、失敗するのは新しい伸長要求と新しいアプリの起動だけ。**推奨 (ユーザー判断)**: 「アプリ 1 本の最小起動量 (PD/PT 16KB + code 最小 + スタック 256KB + heap 64KB ≈ 400KB)」を**起動予約**として台帳に残し、伸長は予約を割り込まない — 前景が池を食い尽くしても gshell から次のアプリ (端末など) を起動できる。
  6. **ユーザー判断が要る点**: (a) 大きな塊の閾値 (`mem_alloc` がヒープで応えるか `mem_map` に回すかの境。提案 64KB 以上は map)、(b) ヒープの縮小 (trim) を実装するか、(c) 起動予約の有無と量。

- **R3 カーネル側が池から取るのは起動時・モジュール読み込み時・そして通常文脈のページ表だけ。** 実行中に要るもの (ドライバのバッファ等) は読み込み時に上限を確保しておく。`kmalloc` は固定の KHEAP (192KB) からで池を借りない (Codex 確認: KHEAP 外へ伸びる経路は無い)。
  - **R3-e ページ表の例外 (Codex 往復 6 P2)**: lease 窓 (PDE 960〜1015) は私有 PT を「要る分だけ」持つので、初めて lease を張る PDE には PT の物理ページが要る。R2 (i)(ii) の伸長でアプリ帯の新しい PDE に触るときも同じ。**契約**: (1) AS 生成時に **lease 窓の先頭 4MB の PT を 1 枚事前確保** (§4-3 の「PT 4〜12」の内側。全画面 lease (VRAM 0xA0000〜、BB ≤ 300KB) はこの 1 枚に収まる)。(2) それを超える PT は**通常文脈で池から 1 枚ずつ取る** (owner=その AS)。(3) **失敗時は操作全体を巻き戻す**: PT が取れなければ lease は張らない (部分写像を残さない)、伸長は上限を動かさない。**lease 台帳は AS ごとの固定配列** (本数の上限は U1 の 16KB の内側で決める) で実行時確保はしない。
  - ドライバの**利用時の割当** (`pcm_open` の `dma_pool_alloc`、`pcm_cs4231.c:525`) は池ではなく固定プールの内側 → R4。

- **R4 連続した物理 — 池からの予約は起動時、固定プール内部の割当は利用時でよい。** (Codex 往復 6 P3 で「予約」と「利用時の割当」を分けた)
  - BB は起動時 (gfx probe の直後、live AS = 0) に池から連続確保 (§2-2)。**池からの新規の連続確保はこれと同梱域・モジュール本体 (読み込み時) だけ** — 断片化で後から取れなくなるのを避ける。
  - DMA プール 64KB (§3-2) は固定の塊として起動時に予約済み。**プール内部の割当 (`dma_pool_alloc` → `dma_alloc(size, align, limit)`) は利用時 (`pcm_open`、82557 の open) の通常文脈でよい** — 池を触らないので R3 に反しない。顧客は PCM リングと 82557 CB/RFD の固定サイズ 2 種なので、最悪の並びで両方入ることを T1 の kselftest (64KB 境界の検査) に加える。PCM の確保時点は変えない。

- **R5 回収の検査 — 通常 AS の終了検査と永続資源を分ける。** (Codex 往復 6 P2)
  - 台帳の owner タグを 2 種に分ける: **AS owner** (アプリの ID。終了で `exec_reclaim_owned(id)` が全部回収) と **永続 owner** (kernel / boot / bundle / shlib / gshell / モジュール名。AS の終了経路は触らない)。
  - **(a) 通常アプリ**: 終了後、AS owner=その ID のページが 0 であることを確かめる (kselftest と `mem` の拡張 — owner ごとの使用量を永続 owner も含めて出す)。
  - **(b) 永続資源**: BB は owner=boot → gshell (永続) の移譲 1 回だけで、GUI 終了 → CUI → GUI 再起動でも保持して再利用する (§2-2)。シェル自身の終了経路 (`exec.c:1367` の `APP_ID_SHELL`、`res_owner 0` の回収) は永続 owner を回収しない。永続 owner の総量は R6 の予算検査 (設計値との一致) で見る — ゼロ検査の対象にしない。

- **R6 カーネルの成長。** カーネルが育つと KERNEL_SLACK が減る。8MB 機の予算の検査 (§4 の勘定と check) が自動で検出する。

- **R7 モジュールの寿命 — 読み込み失敗の巻き戻しと、取り外しの契約。** (Codex 往復 6 P3)
  - **読み込み**: ローダが池から code/data/bss を owner=モジュール名で取り、再配置し、init を呼ぶ。init が池から取るもの (MEMSYS5・代替スタック・KCG ROM のキャッシュ・ドライバのバッファ) も**同じ owner** で取る。
  - **読み込み失敗** (init が非 0、例: MEMSYS5 確保後に代替スタックの確保が失敗): ローダは **owner=モジュール名 の台帳上のページを全部回収**する — 部分初期化の巻き戻しを init の逆順コードに頼らず owner 単位の一括回収に任せる。ただし**台帳の外の状態 (IRQ 登録・DMA の起動・装置レジスタ) は init 自身が失敗を返す前に戻す**。失敗の結果は「未読み込み」と同じ状態 (D21 の MINIMAL と同じ stub) で、起動は続く。T1 の受入「失敗時回収」はこの経路を含む。
  - **取り外し**: **v3 では行わない** (D10「当面は静的」の延長。スプラッシュ (D18) はアプリなので exec の回収)。足すときの契約は **停止 (装置停止・IRQ 解除) → 参照解除 (エクスポート表を stub に戻し、利用者 0 を確認) → owner ごと回収** の順。それまでは kselftest で「モジュールの owner のページ数は読み込み後に増減しない」(R3 の検査) を見る。

#### 3-5-2. R1〜R5 の「通常文脈」

回収・確保・lease の付け外しを許す場所 = **カーネルが自分の状態 (台帳・owner・lease の順序) を中途に持っていない点**:

| 通常文脈 | どこ | 備考 |
|---|---|---|
| syscall の入口 / wrap の中 | `ring3_syscall_dispatch` (`exec.c:1440-1452`) と KAPI の wrap | IF=1 (`int80_stub` が sti 済み)。`ring3_abort_check` はここで畳んでよい |
| exec_run の復帰点 | longjmp の着地、IF=1 に戻した後 | R1 の (b) 回収はここ |
| WM のハンドラ | gshell の OP_WAIT からの `exec_kill(宛先)` | CPL=0 だが syscall の中 (`ring3_wm_depth`)、通常文脈 |
| 起動時・モジュール読み込み | `kernel.c` の初期化列 | live AS = 0 |

**通常文脈でないもの**: IRQ ハンドラ本体 (EOI まで)、割り込みフレームの上で走る `ring3_abort_check` (R1 の (a) 移譲だけ)、V86 の反射中。

#### 3-5-3. P6 (実行モデル) を見直すときに壊れるもの

R1〜R4 はプリエンプティブでない前提をそのまま約束にしたもので、**P6 を見直すときに一緒に見直す**。「見直す」の中身: **カーネル処理中 (KAPI の wrap 実行中・台帳更新中) の強制脱出や AS の切替を許すと次の 3 つが成立しなくなる** (Codex 往復 6 P3):

1. **台帳更新の非再入** — 池の bitmap と owner / lease 台帳は排他なしで更新している (R1 の前提)。途中で抜けると「bitmap は立っているが owner が無い」「owner はあるが PTE が無い」区間が残る。許すなら台帳の更新を割り込み禁止区間か錠で囲む。
2. **owner の安定** — 確保の宛先 `res_owner_get()` は「今の AS」で決まる暗黙の値。確保の途中で AS が切り替わると、取ったページが別の owner に付く。許すなら owner を確保の引数にする (暗黙の current をやめる)。
3. **lease / 回収の順序** — 「lease を全部外してから物理を返す」「shlib detach の後に PD を捨てる」「回収より先に親の文脈へ戻す」(`exec.c:1362`) は、1 本の通常文脈が最後まで走ることで守られている。許すなら回収を再入可能な状態機械にする。

つまりこの 3 つが R1〜R4 の前提そのもの。**R2〜R4 の「起動時・読み込み時に取る」方針はプリエンプションの有無から必然的には決まらない** (Codex) — P6 を見直しても先取りの方針は変えず、変えるのは 1〜3 の守り方だけ。

#### 3-5-4. 断片化 (ユーザーの問い 2026-09-30: 「A が起動後に追加確保 → B を起動 → A に戻って追加確保 → B を閉じる」で断片化しないか)

1. **アプリのメモリは物理連続を要求しない — この例は問題にならない。** アプリ帯は私有写像 (D1・D11) なので、物理ページがばらばらでも A のアプリ帯の中では仮想が連続して見える (PTE が 1 枚ずつ指す)。B の終了で池へ戻ったページも 1 枚単位で誰にでも配れるので、**ページ単位の割り当てには外部断片化が起きない**。R2 の伸長・map/unmap も同じ (物理は 1 枚ずつ、仮想だけ連続)。仮想の側もアプリ帯 448 PDE = 約 1.75GB/AS (`MEM_APP_BAND_MAX_PDES` で 64 PDE = 256MB に絞っても) で RAM より十分広く、仮想の断片化は AS の寿命 (アプリの終了で PD ごと消える) を超えて残らない。
2. **物理連続が要るものの一覧と対処** (これだけが断片化の対象):

   | 何 | なぜ連続 | 対処 |
   |---|---|---|
   | BB (planar 128KB / PEGC 300KB) | gshell が CPL=0・恒等で `rep movs` | 起動時 (live AS = 0) に確保し gshell が永続 owner で保持 (R4・R5) |
   | DMA バッファ (PCM リング・82557 CB/RFD・FDC/ATAPI) | 装置が物理を見る、64KB 境界 | 固定 DMA プール 64KB (0x3E0000) の内側 (R4)。池からは取らない |
   | V86 のゲスト空間 | 実アドレス | 低位の固定アリーナ 0x1000〜0x9FFFF (D13、T7a) |
   | **恒等写像に置くモジュール** (code/data/bss、MEMSYS5、代替スタック) | 池の恒等ページ = 仮想も物理も同じ番地なので、まとまりは物理連続でなければならない | **原則、起動時に読む** (§4-5 の順: 台帳 → 同梱域 (連続、予約済み) → BB → `exec_init` → `/sys/*.mod` → 最初のアプリ)。**v3 のモジュール (SQLite・FEP・PCM・FM・LAN・KCG ROM・NP21/W 専用) はすべて最初のアプリより前に読み、取り外さない (R7)** ので断片化に遭わない。**後から読むモジュールを足すなら** KERNEL_SLACK か台帳の予約域 (連続を保証) から取り、取れなければ読み込みを拒否する (アプリのページを動かさない)。別案「多ページのモジュールをカーネル側の仮想窓で束ねる」は恒等写像を崩す (D20 の P2V/V2P の外) ので**未確認 (U23)** |
   | shlib の text / data 複製 | — | **不要**。ページ単位で各 AS の私有 PT に張る (§2-3 ③) |
   | アプリの PD / PT | — | 1 枚ずつ (4KB 整列だけ) |

3. **アプリの中の断片化** (ヒープの内部断片化) は**アプリのアロケータの領分** (newlib malloc / kheap)。R2 で大きな塊をヒープの外の別領域 (`mem_map`) にすることで、ヒープが大きな穴を抱えて縮められなくなる問題は軽くなる。
4. **順序を R4 に結びつける**: 起動時と読み込みの順は「台帳の初期化 → 連続が要るもの (同梱域・BB・DMA プールの予約・モジュール本体) → 通常 (ページ単位の配布 = 最初のアプリ以後)」。**連続が要る確保は全部、最初のアプリより前に終える** — これが R4 の「池からの新規の連続確保は起動時・読み込み時だけ」の理由で、8MB 機でモジュールが入る根拠 (§4-3) はこの順序の上で成り立つ。

## 4. SQLite・FEP・モジュール・ブート順

### 4-1. いまの依存 (事実)

- リンク: `build/os32.ld:60-93` が 0x200000 に配置、`kernel.mk:190-201` が `sqlite.bin` を VK32 に同梱。カーネル側の直接シンボル参照は `kapi/kapi_db.c` (19KB) と `kernel/ime_dict.c` (`:5` に「KAPI 不使用」)。
- 大きさ: `sqlite3.o` text 367,146 + data 3,760 + bss 789、`os32_sqlite_vfs.o` bss 394,628 (MEMSYS5 384KB、`os32_sqlite_vfs.c:34-39`)、代替スタック 128KB。
- **再配置の実態** (`readelf -r`): 内部の `R_386_PC32` が大半 (sqlite3Vdbe* 1,123 件など) のほか、**外部関数への `R_386_PC32`** (`memset` 175、`memcpy` 171、`vfs_validate_sqlite` …) と **外部データへの `R_386_32`** (`tick_count` × 3、`os32_sqlite_vfs.o`) がある。絶対参照の一覧にベースを足すだけでは足りない (Codex P2-2)。
- `sqlite3_config(SQLITE_CONFIG_HEAP, ptr, size)` は任意ポインタ可 (`:633-635`)。

### 4-2. モジュール形式 (D12・D17)

| 段 | 内容 |
|---|---|
| ビルド | `ld -r` で 1 本にまとめず、**ベース 0 で最終リンク** (`-Ttext=0 --emit-relocs`、モジュール用 `.ld`)。`--emit-relocs` は**解決済みの内部 PC32 も出力する**ので、`mkmod.py` は (1) 参照先がモジュール内 (シンボルがモジュールのセクションに定義) の `R_386_PC32` は**位置独立なので検証して実行時表から除外**、(2) モジュール内への `R_386_32` は実行時表へ、(3) 参照先がモジュール外のもの (未解決) と (1)(2) 以外の種別は**拒否**、の規則で表を作る。外部関数はインポート用スタブ経由なので (1) に落ちる。**メモリサイズ = text+data+bss を整列込みで確定**し、ヘッダに `mem_size` と `bss_zero` 範囲を持つ (画像長 `raw_size` だけで隣を決めない) |
| 外部関数 | モジュールは外部関数を直接呼ばない。`tools/mkmod.py` が**インポート表 (名前の並び) とスタブ (`vfs_validate_sqlite: jmp [slot_n]`)** を生成してリンクに混ぜる。`memset` / `memcpy` も同じ (モジュール内に持つか、スタブ経由、U22)。カーネルのローダは slot にカーネル側の実番地を書く |
| 外部データ | **禁止**。`tick_count` は `ktime_ticks()` のアクセサに置き換える (`os32_sqlite_vfs.c` の 3 か所)。`mkmod.py` は外部データへの `R_386_32` を見つけたら**生成を拒否** |
| 再配置表 | `R_386_32` の位置一覧 (4B × 件数。SQLite は数千件 = 十数 KB、推測)。ローダはベースを加算。**表に無い種別が残っていたら拒否** |
| エクスポート表 | カーネルが呼ぶ入口 (`sqlite3_open_v2` … `db_mem_used` が使う `sqlite3_memory_used`)。`kapi_db.c` / `ime_dict.c` は `os32_sqlite_vfs.h` のマクロ層で表を引く |
| 置き場・メモリ | 池 (第一候補 KERNEL_SLACK)、CPL=0、supervisor。MEMSYS5 (8MB は 256KB (推測、U3)、他は 384KB) と代替スタック 128KB は**モジュール init が池から取り** `SQLITE_CONFIG_HEAP` へ。`MEM_SQLITE_STACK_TOP` は変数に |
| 未読み込み時 (MINIMAL だけ、D21) | `db_open` は `OS32_ERR_NOSYS` 系の 1 値。FEP は §5-6 の**API 別 stub** |
| 池からの予算 | code 375 + MEMSYS5 256〜384 + 代替スタック 128 + 再配置表 ≈ **775〜903KB** |

### 4-3. 8MB の勘定 (D9・D21)

構成: **コアだけの本体 (§4-4) + モジュール読み込み (FEP・KCG ROM・PCM・FM+snd)** + SQLite モジュール (MEMSYS5 384KB = 上限側) + OpenType (§5-4: フォント表とキャッシュは gshell はシェル帯、アプリは私有総量の内側)。**私有総量 2,048 には shlib data 複製 40 と既定スタック 256 を含める**。固定 = 5,120KB、池 = 3,072KB + KERNEL_SLACK。**SQLite と FEP と KCG ROM fallback は読み込んだ構成** (D14・D21) が標準。

| 池から取るもの (KB) | PEGC | planar | 出典 / 注 |
|---|---:|---:|---|
| アプリの私有総量 (code+bss+sbrk+exec_heap+スタック+shlib data 複製) | 2,048 | 2,048 | D9 |
| shlib 共有 (text 104 + data 40 + 原本 40) | 184 | 184 | Codex 往復 2 ③ (現行実測、再ビルドで変わる) |
| アプリの PD + PT (帯 1 枚 + lease 窓 1 枚) | 16 | 16 | PD 4 + PT 4〜12 |
| 台帳の bitmap (2048 PFN、4KB) + workspace 1 ページ + **owner/lease 台帳 (推測 16KB、U1)** | 24 | 24 | `pgalloc.c:87`、`memory_boot.c:15` |
| バックバッファ (owner=boot → gshell) | 300 | 128 | `MEM_GFX_BB8_SIZE` / `MEM_GFX_BB_SIZE` |
| SQLite モジュール (code 375 + MEMSYS5 384 + 代替スタック 128 + 再配置表 ≈ 16) | 903 | 903 | §4-2 |
| モジュール: FEP 19 + 状態 3、KCG ROM 4、PCM 7、FM+snd 22、再配置表・ページ切り上げ ≈ 24 | 80 | 80 | §4-4 の `size` 群計、切り上げは推測 |
| KCG ROM fallback のキャッシュ (LRU 64KB、§5-5、D14) | 64 | 64 | Codex 往復 4 P3 で計上 |
| フォント (OpenType): gshell の表 100 + キャッシュ 80 は**シェル帯の内側**、アプリのは私有総量の内側 | 0 | 0 | §5-4 |
| Unicode 組表 28KB: カーネル `.rodata` (SLACK が減る側で計上) | 0 | 0 | §5-3 |
| **合計** | **3,619** | **3,447** | |
| **余り: 池 3,072 + SLACK 2,118 (コアだけ、推測) = 5,190** | **+1,571** | **+1,743** | **これが D21 の「+1.57〜1.74MB」** |
| 参考: SLACK を今の本体で見る (1,972 − 28 = 1,944 → 池 5,016) — この列は FEP 等が本体に残るので二重計上気味 | +1,397 | +1,569 | Codex P3 の算定 |
| 参考: SLACK を返さない (池 3,072) | −547 | −375 | 返却は 8MB で必須 |
| 参考: KCG 形式のキャッシュを池に置く旧案 (292KB、ページ確保) | +1,343 | +1,515 | `.kcgfont` 廃止 (D14) で不採用 |
| 参考: MINIMAL (FEP + SQLite を読み込まない、英数のみ) | +2,496 | +2,668 | 回復用構成だけ (D21) |

V86 (低位を直接使う) と GUI は同時に走らない (`OS32X_FLAG_CUI_ONLY`、`exec.c:1743-1747`)。**両方式とも 1MB 級の余裕が見込める**が、余りの確定値は T3 の実測 (`pgalloc_free_pages`) に置き換える。

### 4-4. コアとモジュールの区分 (D17) — 大きさ

`build/kernel.mk` の C オブジェクトを `size` で群に分けた実測 (asm・`kapi/` 生成物・整列を含まないので群の合計 558KB と 583KB の差 ≈ 25KB がそれ):

| 群 | .o | text+data | bss | 計 | 行き先 |
|---|---|---:|---:|---:|---|
| **コア** (PIC・PIT・クロック判定・DMA・ページング・8251・IDE/HDD・区画表・ext2・console・RTC・シリアル・**FDC**・exec・VFS・gfx core・9801/PEGC バックエンド・kapi) | 95 | 196.0 | 163.7 (静的 PT 40 は固定の塊へ) | **359.7** | カーネル帯 |
| コア: kselftest | 1 | 20.9 | 2.4 | 23.3 | カーネル帯 |
| M: V86 モニタ | 8 | 18.8 | 8.4 | 27.2 | モジュール (CUI のときだけ) |
| M: FM + snd_engine | 2 | 7.0 | 14.7 | 21.7 | モジュール |
| M: PCM CS4231 | 2 | 6.5 | 0.1 | 6.6 | モジュール |
| M: LAN (NE2000 / LGY-98 / link) | 4 | 19.0 | 21.6 | 40.5 | モジュール |
| M: FEP | 4 | 15.3 | 3.2 | 18.5 | 案 A (静的 + 表経由、T4) → 案 B (モジュール、T5a)。**常に読み込む** (D21) |
| M: KCG + boot_font + boot_splash | 3 | 3.6 | 0.6 | 4.2 | KCG ROM は予備として残す (D14)、boot_font は `.kcgfont` 廃止で消滅、**スプラッシュはアプリへ** (D18) |
| M: FAT / iso9660 / ATAPI / loop / diskio | 6 | 34.6 | 3.5 | 38.1 | モジュール (**ブート必須のときは同梱、§4-5**) |
| N: HostDrv / np2sysp + シームレスマウス / Cirrus + WAB | 6 | 12.9 | 4.9 | 17.9 | NP21/W 専用モジュール (実機では読まない) |
| (別枠) SQLite | 3 | 374.6 | 395.4 | 770.1 | §4-2 |

コアだけの本体 ≈ 383 + 25 − 40 (PT) + 28 (Unicode 組表) ≈ **≈ 396KB** (推測 ±10KB)。鎖の終端 ≈ 0x100000 + 396 + 428 = 824KB → **KERNEL_SLACK ≈ 2,944 − 824 ≈ 2,120KB** (§4-3 は 2,118 で計算)。

### 4-5. ブート順とローダの区間表 (D18)

**事実**: ルートマウントは `kernel.c:434` で、`paging_init` (`:505`) と `memory_boot_init` (`:558`) より**前**。FD 起動のルートは `fat` (`:425-427`)。VK32 は最大 4 エントリ、展開先は 0x100000〜0x2E8000 (`boot_defs.h:44,57-58`)、集積は 0x10000〜 (508KiB、`:93`)。

**2 段階の起動順**:

1. ローダ: 圧縮画像を**集積域**へ読む → CRC → 展開 (カーネル → カーネル帯、同梱モジュール → **同梱域**)。
2. カーネル (PG=0 でも動く順): メモリ検出 → `paging_init` → **台帳初期化 (集積域は解放、同梱域は owner=bundle で予約)** → **必須モジュール (ブート FS: FAT / iso9660+ATAPI、必要なら HostDrv) をその場で再配置・登録** (コピーしない、同梱域を最終置き場にしてよい) → **ルートマウント** → gfx probe + BB 確保 → `exec_init` → 通常モジュール (`/sys/*.mod`: SQLite → FEP → PCM …) → **スプラッシュ (最初のアプリ、終了でアンロード)** → シェル (`exec_run`、シリアルの会話 (rshell / SerialFS) はここから、D18)。
   - `paging_init` / 台帳をルートマウントより前に動かす (今は逆)。動かせない事情があれば (U19)、同梱域は**固定番地の静的予約**として台帳初期化時に除外するだけで同じ効果 (必須モジュールは PG=0・恒等で再配置できる)。
   - HDD (ext2 ルート) の構成では同梱は無し (ext2 はコア)。**FD 起動の最小構成 = コア + FAT の 2 エントリ**。MINIMAL (D21) はこれに SQLite / FEP のモジュールを読まない構成。

**区間表 (8MB 機で成立、全構成で同じ番地)**:

| 区間 | 用途 | 上限 | 寿命 |
|---|---|---|---|
| 0x010000–0x08EFFF | ローダの集積 (今) | 508KiB | T6b まで。**それ以後は集積を 0x500000〜へ** |
| 0x0009FFFC 以下 | ローダのスタック・FAT バッファ・bootinfo | — | カーネル起動まで |
| 0x100000–0x3DFFFF | カーネル本体 + 鎖の展開先 (`VK32_LOAD_MIN/END` をここへ) | 2,944KB | 常駐 |
| **0x500000–0x5FFFFF** | **集積域** (T6b 以後の圧縮画像) | **1MB** (mkvk32 が拒否) | 展開が終わるまで。台帳初期化で池へ |
| **0x600000–0x6FFFFF** | **同梱域** (ブート必須モジュールの展開先、ページ整列) | **1MB** (mkvk32 が拒否) | 台帳が owner=bundle で予約。モジュールの並びは **`mem_size` (BSS・整列込み、ヘッダの値) で決め、mkvk32 は BSS をゼロ込みで画像に含めるか `bss_zero` 範囲を書く** (VK32 の範囲検査は `raw_size` だけ `vk32_boot.c:85`)。再配置・登録 (BSS ゼロ化を含む) の後に、`mem_size` の外の余りページだけ池へ (その場で使うので二重保持なし) |
| 0x700000–0x7FFFFF | 池 (8MB 機ではここまで) | — | — |

非重複: 集積 [0x500000, 0x600000) と展開先 [0x100000, 0x3E0000) ∪ [0x600000, 0x700000) は素 — `vk32_boot.c:86-92` の「読み込み域は帯の下」を**明示の非重複検査**に置き換え、集積域の上限もヘッダで検査。展開時のピーク = 集積 ≤ 1MB + カーネル ≤ 2.5MB + 同梱 ≤ 1MB < 8MB。圧縮画像の見込み: 本体 ≈ 208KB (`lz4 -9` 相当、U2) + FAT モジュール数十 KB → 508KiB の中なので **T6b (508KiB 解除) は急がない**。

---

## 5. Unicode 表・フォント・FEP・バックバッファ

### 5-1. Unicode 表の利用者 (事実)

| 利用者 | 経路 | 出典 |
|---|---|---|
| 読み込み | `/sys/unicode.bin` (131,072B) を物理 0x4A000 へ | `kernel.c:604-618` |
| 実体 | `lib/utf8.c:32` が直接索引、4 点照合 `:65-75` | |
| TVRAM コンソール | `console.c:327,420` (TVRAM は JIS) | |
| FEP の TVRAM 描画 | `ime_render_tvram.c:40` | |
| GUI 文字 | `gfx_kcg.c:98` → KAPI `kcg_read_kanji`、`lconsole.c:234`、libos32gui `draw.rs:568` | |
| KCG (ROM / キャッシュ) | 字形の索引が JIS | `kcg.c:127-` |
| アプリ | 0x4A000 を USER で直読 (`exec.c:1944-1947`) | `game/app/main.c:183-205` |
| kcg.c の LZ4 一時バッファ | 同じ番地を流用 | `kcg.c:207-216` |

### 5-2. 今のフォントキャッシュ (事実)

295,940B (`kcg.c:47-50`) を `/sys/font/default.kcgfont` (188,110B、`gen_font16.py` が IPAex から焼いた 16px 1bpp) から起動時に全字展開。**アプリの字形取得は KAPI 1 字 1 トラップ** (`gfx_kcg.c:31,50`)。ユーザ側で 0x1000 を直読する利用者は grep で見つからない (U12)。

### 5-3. Unicode 表 — 表は要るが 128KB は要らない

`lib/unicode_jis_table.h` に **7,063 組 (u16,u16) ソート済み 28,252B** が既にあり (`:2`)、`unicode.bin` はその展開。→ **カーネル `.rodata` + 二分探索**。`/sys/unicode.bin`・ready 契機・4 点照合・LZ4 流用・CLAUDE.md §4-11 の罠が消える。アプリ側は KAPI 1 本 (`unicode_to_jis`、[ABI2] 追記) か shlib に置く (T7a で決定)。**KCG を予備に落としても表は消えない** (TVRAM コンソールと FEP の TVRAM 描画が JIS)。SLACK が 28KB 減るのは §4-3 に計上済み。

### 5-4. フォント — OpenType (D14: `.kcgfont` 廃止)

材料: `font_test` (ttf-parser 0.21 + ab_glyph_rasterizer、107KB) はファイル全体を `Face::parse` に渡し、ttf-parser は `glyf` のスライスを保持する (`lib.rs:255,263`) → **`lseek` 置換だけでは動かない**。サブセット TTF: 7,144 グリフ、`glyf` 3,231,617B (平均 452B、**最大 1,076B、複合 0**、Codex 実測)、`loca` 28,580B、`cmap` 40,496B、`hmtx` 28,572B。

| 項目 | 設計 |
|---|---|
| ストリーミング読み出し層 (**実装項目**) | 常駐する表: `cmap` `loca` `hmtx` `head` `hhea` `maxp` ≈ 100KB。`glyf` は字形ごとに `sys_lseek` + `read` (loca から offset/length)。ttf-parser には**字形 1 つ分のスライス**を `glyf` として渡す薄い層を書く (ttf-parser の `Face` 全体を使わず `glyf::Table` 相当を自前で組む、推測: 数百行)。作業域: 輪郭 ≤ 2KB (このファイルは 1,076B、**OpenType 一般の保証ではない** → 上限超えは字形を「□」に落とす)、累積面 16px 1KB / 32px 4KB |
| ラスタライザとキャッシュの所有者 | **v3 の最初は私有**: libos32gui (shlib、text 共有) にラスタライザを置き、**キャッシュはアプリの shlib data / ヒープに私有** (私有総量の内側、16px 1bpp 2,048 字 × 40B = 80KB)。**gshell は libos32gfx を静的リンクしている** (`gshell/Cargo.toml:12`) ので、WM も同じ Rust クレートを静的に持ち、**シェル帯の内側に**表 100KB + キャッシュ 80KB を私有で持つ (gshell 219KB + 180KB + ヒープ < 1MB、U15)。CPL=3 の shlib から supervisor の共有キャッシュには書けないので、**共有キャッシュ (gshell 所有、RO lease、欠けた字形は GUI op で WM に頼む) は後の最適化** (U21) |
| 予算 | 8MB: 16px 1bpp 64〜80KB/プロセス。16MB 以上: 256KB (2 サイズ or 2bpp) |
| 判定 | 1x の見た目 (罠 POLICY_DEBUG §4-10)、ラスタ時間 (U4: P100 で 1 字 0.2〜1ms、386 は 10 倍以上 — 推測)。cold の 1 画面 1,000 字 = 0.2〜1 秒 |
| `.kcgfont` | **廃止** (D14)。`gen_font16.py` / `boot_font.c` / `kcg_load_font` KAPI (スロットは残し NOSYS) は消える |
| **KCG ROM fallback (予備、D14 で決定)** | **残す (モジュール 4KB)** — フォントファイルが読めない構成 (FD 起動・`/sys` 破損) でも GUI の文字が出る唯一の手段で、コストは 4KB と JIS 表 (どのみち残る) と読み出しキャッシュ 64KB (§4-3 に計上) |

### 5-5. 低位 640KB の解放 (D13)

| 今 | v3 |
|---|---|
| フォントキャッシュ 292KB (0x1000) | 消滅 (OpenType は私有)。KCG ROM fallback の読み出しキャッシュは**モジュールが池から取る** (LRU 64KB を上限に、§4-3 に計上。別案 = キャッシュを持たず毎回 ROM から 32B 読む: I/O 34 本/字 (`kcg.c:147-165`) で遅いが 0KB、推測) |
| Unicode 表 128KB (0x4A000) | カーネル `.rodata` 28KB |
| GFX BB (9801、128KB、0x6A000) | 池へ (owner=boot → gshell、§2-2) |
| 空き 88KB (0x90000 mailbox) | mailbox は残す |

残るのは bootinfo と mailbox → **V86 は低位を直接使える** (`v86_mem.c:44` の 636KB バッキングの撤去は T7a で判断、U7)。

### 5-6. FEP (D21: 常に読み込む)

事実: カーネル静的 (`kernel.mk:31`、`kernel.c:624`)、18.5KB、辞書 5.5MB はディスク、MEMSYS5 取り分 38KB (`DESIGN.md:166`)、SQLite を直接呼ぶ唯一の利用者 (`ime_dict.c:5`)。gshell は `ime_is_active() != 0` を「有効」と読み (`fep.rs:326`)、`ime_feed_key` の負値を「消費」と読む (`fep.rs:388-391`、`ime.c:751-758`)。

| 問い | 決定 / 案 |
|---|---|
| モジュール化 | **A**: 静的のまま SQLite だけ表経由 (T4 の中)。**B**: `/sys/fep.mod` (SQLite に依存、T5a)。A → B の順 |
| 依存 | FEP → SQLite (辞書)、→ 描画 (TVRAM は JIS 表、GUI は gshell の表)、→ キーボード (KAPI 経由、ISR 境界なし、推測) |
| **未搭載時の stub (API 別)** — MINIMAL でだけ効く | `ime_is_active` = 0、`ime_get_mode` = 0、`ime_set_mode` / `ime_toggle` / `ime_set_render` / `ime_switch_dict` = 無操作 (0)、`ime_feed_key(k)` = **素通し (k ≥ 0 はそのまま返す、k < 0 は 0)**、`ime_getkey` / `ime_getchar` = 生のキー入力に落とす (ブロッキング)、**`ime_trygetkey` = `kbd_trygetkey()` の生キー (非ブロッキング、無ければ −1)、`ime_trygetchar` = その文字** (CUI エディタの `vz_kbhit` が `ime_trygetkey` を回している `apps/edit/sys_keyboard.c:27`、`ime.c:851`)、`ime_user_*` = `OS32_ERR_NOSYS`。スロット順は維持 |
| 読み込まない構成 | **決定 (D21)**: SQLite と FEP は常に読み込む。例外は **MINIMAL (FD 起動の回復用構成) だけ**。標準 8MB の受入は FEP 込み。**受入に「MINIMAL で英数の入力・編集 (gshell のテキストボックスと CUI エディタ)」を追加** |
| 8MB への影響 | FEP 22KB、SQLite 込み 925KB (§4-3) |

### 5-7. バックバッファの持ち主とサーフェス層 (D19)

**事実**: 9801 = 主記憶 BB 128KB + CPU 転送 + ページフリップ (`gfx_vram.c:206-232`)。PEGC = 主記憶 BB 300KB + CPU が F00000h の窓へコピー (`backend_pegc.c:916-955`)、エンジン無し、VRAM 512KB で 480 ラインは 1 面 (`02_memory.md:51-53`)、**A8000h のバンク窓もある** (`backend_pegc.c:5-7`)。Cirrus = カード VRAM の非表示面 + エンジン BLT (`backend_cirrus.c:436-453`)。exec が BB (`exec.c:1955-1959`) と VRAM (`:1932`) を全アプリに USER で張る。WM の読み戻しはカーソルの下地だけ (`cursor.rs:90`、BB から)。ドラッグ枠は BB に描いて再合成で消す (`chrome.rs:348-352`)。VRAM は読まない設計 (gui/DESIGN §8)。

**(A) PEGC で BB を持たない案** — WM には不採用: G4 と R3 に反し、カーソル下地が VRAM 読み戻しになり、消去がちらつく。画面外 212KB はエンジンが無いので VRAM→VRAM が主記憶→VRAM より必ず遅い (推測、DESIGN §8 から)。8bpp 400 ライン (256,000B × 2 面) のフリップだけ価値があり、それは**全画面 lease (P9)** の話 → **PEGC 直描きは全画面 lease に限る** (D19)。Cirrus は「BB が VRAM の中」なので、揃えるのはサーフェス層 (置き場をバックエンドが決める)。planar の BB は池へ。

**(B) 持ち主** — **決定は (b)** (v3 の最初)。(c) はサーフェス層の票で。

| 方式 | BB | 置き場 | PEGC / planar / Cirrus | 8MB | 得失 |
|---|---|---|---|---|---|
| (a) カーネル (今) | 要 | 池、全 AS に共有 USER | 300 / 128 / 0 | §4-3 | 共有 PT の USER が残る (P1-1 の反例) — 不採用 |
| **(b) gshell** (決定) | 要 | ブート時に台帳から連続確保 → gshell へ移譲 (§2-2)。アプリへは lease 窓に別名写像 | 300 / 128 / 0 | §4-3 と同じ | exec の特例が消える。present は gshell (CPL=0) が窓へ直接、Cirrus は BLT 呼び出し。**v1 の描画モデル (R1〜R3) を変えない** |
| (c) アプリ私有サーフェス + WM 合成 | 要 (WM) + アプリ面 | アプリ面は私有総量の内側、WM の BB は (b) | 300+面 / 128+面 / 0+面 | 池 ±0、2MB の実効 −120〜300 | G4 が構造で成り、アプリに lease が要らない。Paint ごとに WM のコピー 1 回 (U16)。**サーフェス層の票で** |
| (c') (c) + WM が VRAM へ直接合成 | 不要 | — | 0 / 0 / 0 | −300 / −128 | 再合成の途中が見える (U18)。planar は不可 (4 プレーン変換が VRAM 直で 2 倍)。PEGC のみ |
| 全画面アプリ (P9) | 不要 | 表示面 / フリップ面を lease | 0 | 0 | `fullscreen.rs` の経路が既にある |

**(C) DirectDraw 的な層** — 既存の `GfxBackend` + 能力ビット (`GFX_CAP_*`、`os32_kapi_shared.h:275-277`) + libos32gfx がほぼ対応。足りないのは**台帳の SURFACE 種別と lease 契約 (§2-2)**。カーネルの最小限 = モード設定 / probe / enter / leave、窓の写像 (supervisor+PCD)、present / flip / VSYNC、能力表、HW エンジンの薄い口、**lease**。外へ出すもの = BB の所有 (gshell)、描画 (libos32gfx)、合成・カーソル・ドラッグ枠・FEP 窓 (gshell)。カーネル予算への効果は小 (gfx/ 28KB の一部)、効果は構造。**音 (PCM リング = D5 の `dma_alloc` で済む) と入力は P8、Video HAL / VESA2 的 / SDL は P9 (`docs/DESIGN_APP_FIRST.md:134-160`) — 別の柱 (D19)。**

---

## 6. 票の分割と順序 (T0〜T7)

| 票 | 内容 | 受入の要点 |
|---|---|---|
| **T0 C11** | `_Static_assert` へ (V3_PLAN_DRAFT P0) | 全ビルド + check |
| **T1 台帳** | 物理地図 + 所有権台帳 + **SURFACE / lease の型**、池を `MEM_POOL_BASE` から model 経路で全 RAM 量に (legacy 撤去)、owner タグ、`dma_alloc`、**集積域・同梱域の予約規則**、`sys_reserve_top` 撤去、BB のブート時確保と移譲、**P2V / V2P の導入と物理ポインタ直接参照の監査 (§3-4、D20) + `check_constraints.py` の検査**、**§3-5 の台帳側: owner を AS / 永続の 2 種に (R5)、割り込み中の確保・解放を数える検査 (R1、T2 で panic に)、モジュール owner の一括回収 (R7)、DMA プール内の最悪の並び (R4)** | kselftest: 予約・割当・解放・失敗時回収 (モジュール init の途中失敗を含む)、64KB 境界 (PCM リング + 82557 の同居)、8MB/17MB/64MB、P2V 検査 0 件、割り込み中の台帳操作の件数を報告 |
| **T2 アプリ帯 + lease 窓** | **設計段で lease 契約 (§2-2) を先に決める** → 0x80000000 へ、lease 窓 0xF0000000、定数分離、claim / DEVICE_FLOOR / `--cpl0` 撤去、`exec.c:1932-1959` の共有 USER 写像撤去 (SHM・トランポリン以外)、`paging.c:796` の共有 PT 経路撤去、shlib を池 + AS ごと写像、既定 heap、**スタックを可変 (OS32X ヘッダ)**、検査 3 段 (§2-3 ⑥)、app.ld / shlib.ld / mkshlib / stub.rs、全再ビルド・旧形式拒否、**BB を gshell 所有 + lease に (b)**、**§3-5 の exec 側: 強制脱出を移譲 / 回収の 2 段に (R1)、伸長の KAPI (`brk` 相当 + `mem_map` / `mem_unmap`、R2 — ユーザー確認後)、lease 窓の PT 1 枚の事前確保と失敗時の巻き戻し (R3-e)** | CPL=3 起動・終了・fault・複数 AS、旧バイナリ拒否、**lease を 2 AS に貸して互いに見えない**、通常 GUI アプリが VRAM を読み書きすると kill、**CUI の無限ループ中の CTRL+STOP と PCM 再生中の CTRL+STOP で池の空きが戻る (R1)**、伸長 → unmap → 終了で owner のページ 0 (R2・R5) |
| **T3 カーネル帯** | 3MB + 固定の塊 + KERNEL_SLACK + シェル 0x400000 (1 箱) + DMA 整列 + PT を画像の外へ + Unicode 組表を `.rodata` へ (T7a から前倒し可)。SQLite は本体の直後に連結 | 地図検査、8MB の予算検査、`pgalloc_free_pages` の実測で §4-3 を更新 |
| **T4 SQLite 分離** | §4-2 (ベース 0 リンク + emit-relocs、インポートスタブ、外部データ禁止、未対応再配置の拒否)、MEMSYS5 / 代替スタックを池へ、`kapi_db` / `ime_dict` を表経由 (FEP 案 A)、MINIMAL での `db_*` | `db_*` 全 KAPI、FEP 変換、`db_mem_used` の戻り、MINIMAL で起動 |
| **T5a FEP** | 案 B (`/sys/fep.mod`)、**API 別 stub** | FEP 有り / MINIMAL の両方で英数入力・編集、FEP で 1 語変換 |
| **T5b ブート必須 FS の同梱・早期ロード** (**T5c より前**) | 2 段階の起動順 (§4-5)、`paging_init` / 台帳をルートマウントの前へ (U19)、同梱域、mkvk32 の複数エントリ、FAT / iso9660+ATAPI / HostDrv の切り出し | **FD 起動 (コア + FAT 同梱の 2 エントリ)**、CD 起動、HDD 起動 (同梱なし) |
| **T5c その他のモジュール** | V86 / FM+snd / PCM / LAN / KCG ROM (予備) / NP21/W 専用 3 種、一覧ファイル、supervisor 検査、**スプラッシュをアプリへ** (D18) | 実機で NP21/W 専用を読まずに起動、コアだけの本体サイズを記録、スプラッシュ → シェルの順 |
| **T6b ローダ 508KiB 解除** | 集積を 0x500000〜 (上限 1MB)、非重複検査 | 8MB での展開ピーク、FAT 経路 |
| **T7a 低位解放** | BB (planar) を池へ、V86 の低位直接使用、フォントキャッシュ (0x1000) の撤去、`.kcgfont` の撤去 | V86 起動 (CUI)、GUI と交互 |
| **T7b OpenType** | ストリーミング読み出し層、libos32gui + gshell のラスタライザ、私有キャッシュ、KCG ROM fallback (予備) | 1x の見た目、cold/warm の描画時間 (D15) |

旧本文 (§11-2) の B1〜B7・B9 は T2/T3 で閉じ、**B8 は T5b (同梱) + T6b (508KiB)**。順序: T0 → T1 → T2 → T3 → T4 → T5a → T5b → T5c → T6b → T7a → T7b (T7a の Unicode 組表だけ T3 へ前倒し可)。V3_PLAN_DRAFT §3 の P1 (この票) → P2 (モジュール = T4〜T5c) → P4 (デバイス窓の資源割当 = T1 の台帳の上) の順と一致する。

---

## 7. 受入条件

| 構成 | 条件 |
|---|---|
| **NP21/W 8MB × 9801 planar / × PEGC** | 起動 kselftest 0 fail、地図検査 3 段 0 件、gshell 起動、**私有総量 2MB の試験アプリ 3 種** (内訳: (i) code 1,500KB + shlib data 40KB + スタック 256KB + ヒープ 252KB = 2,048KB、(ii) スタック 512KB (可変) + code/bss 64KB + shlib data 40KB + ヒープ 1,432KB = 2,048KB、(iii) code 200KB + shlib data 40KB + スタック 256KB + exec_heap 1,552KB = 2,048KB) の起動・描画・終了 ×20 で池の空きが戻る、FEP で 1 語変換、**MINIMAL で英数の入力・編集 (gshell のテキストボックスと CUI エディタ `edit` の両方)**、fault_kill 復帰、**CUI の無限ループ中の CTRL+STOP で回収され池の空きが戻る (R1)**、**伸長 (R2) で池を使い切っても gshell が生きて次のアプリを起動できる**、GUI 終了 → CUI で `v86`、余りを `pgalloc_free_pages` で実測して票へ |
| NP21/W 17MB | 上と同じ + 15〜16MB が池に無い + 2 枚以上の PDE を使うアプリ |
| 実機 Ra266 64MB | 上と同じ + PCI BAR の窓が sup+PCD + PCM 再生中の操作 + D15 の判定 (数字を票へ) |
| FD 起動 (NP21/W と実機) | コア + FAT 同梱で `/` がマウントでき、シェルまで上がる。MINIMAL で `db_*` が決まった誤りを返す |
| 全構成 | 旧形式 (load_addr 0x500000 / `--cpl0` / 旧 shell.bin / 旧 shlib / 旧 `.kcgfont`) の拒否、DMA 64KB 境界、圧縮画像の上限 (T6b 前 508KiB / 後 1MB) と非重複検査、変異 (境界を 1 ページずらす) をリンク ASSERT と地図検査が止める |
| 保護 | ring3_guard: アプリから (a) シェル帯、(b) 池の他 owner、(c) 自分の PT、(d) 固定 PT、(e) **VRAM (テキスト・プレーン・バンク窓・リニア窓)**、(f) 他 AS に貸したサーフェス を触ると kill されて OS が生きる。(g) SHM・トランポリン・自分の lease は読めて生き残る。**通常 GUI アプリは VRAM に触れない** |

---

## 8. Codex 指摘の解消 (往復 3・4)

### 8-1. 往復 3 (最終案 v1 への指摘) → v2

| # | 指摘 | 対応 | 場所 |
|---|---|---|---|
| P1-1 | 共有 PT のままでは 1 PD だけへの lease が作れない (`paging.c:796`) | **反映**: lease 窓 0xF0000000〜 (PDE 960〜1015、AS ごとの私有 PT) に別名写像。低位の恒等は supervisor のまま。`addrspace_map_user_page` の共有 PT 経路を撤去。検査は「その AS の lease なら可」 | §2-1、§2-2、§2-3 ⑥ |
| P1-2 | VRAM の常時 USER 写像 (`exec.c:1932`) が全画面 lease を迂回 (planar の A8000h、PEGC のバンク窓) | **反映**: 通常 GUI アプリへの VRAM 写像を撤去。VRAM は全画面 / CUI の TVRAM / V86 の用途別 lease。受入 (e) を「VRAM に触ると kill」に反転 | §2-1、§2-2、§7 |
| P1-3 | ルート FS モジュールの起動順が逆、T5 → T6 の順序が逆 (`kernel.c:425`) | **反映**: 2 段階 (台帳 + 同梱域予約 → 必須モジュール登録 → ルートマウント → 通常モジュール)。同梱・早期ロードを T5b として T5c より前へ。`paging_init` を動かせない場合の代替 (静的予約) も書いた | §4-5、§6 |
| P2-1 | ローダの 8MB 以内の証明、区間と寿命 | **反映**: 区間表 (集積 0x500000〜 1MB、同梱 0x600000〜 1MB、上限は mkvk32 が拒否、その場で再配置して二重保持なし) | §4-5 |
| P2-2 | SQLite モジュールは絶対参照だけでは未完成 (`R_386_PC32` 外部、`tick_count`) | **反映**: ベース 0 で最終リンク + `--emit-relocs`、インポートスタブ、外部データ禁止 (`tick_count` → アクセサ)、未対応再配置の拒否 | §4-1、§4-2 |
| P2-3 | FEP 未搭載時の一律エラーは英数入力を壊す (`fep.rs:326,388`) | **反映**: API 別 stub、受入に英数の入力・編集 | §5-6、§7 |
| P2-4 | OpenType は lseek 置換だけでは動かない、キャッシュの所有者、WM の到達経路 | **反映**: ストリーミング読み出し層を実装項目に、キャッシュは v3 の最初は私有 (アプリ = shlib data、gshell = 静的リンク)、共有キャッシュは後 (U21)。最大字形 1,076B を採用しつつ「一般の保証ではない」 | §5-4 |
| P2-5 | BB の先行確保と gshell 所有の接続 | **反映**: ブート時に連続確保 → gshell へ所有権移譲、GUI 終了後も保持 | §2-2 |
| P2-6 | 私有総量 2MB と固定 256KB スタック、shlib data を数えるか | **反映**: スタックは既定 256KB で OS32X ヘッダから可変。shlib data 複製は総量の内側。受入アプリ 3 種 | §2-1、§4-3、§7 |
| P3 | 数値の補正 (SLACK 1,972、Unicode −28、KCG 292、二重計上)、票の分割、B1〜B9 の矛盾 | **反映**: 表を「コアだけ + モジュール読み込み」の 1 構成で作り直し (参考行に Codex 算定 1,972 も併記)、Unicode は SLACK 側で減算、KCG 形式は参考行に格下げ、票を T5a/b/c・T6b・T7a/b に分割、B8 = T5b + T6b と明記 | §4-3、§6 |
| — | KCG ROM fallback は決定済み扱いにしない | 当時「未決」に戻し推奨だけ → **2026-09-30 ユーザーが「予備として残す」を承認** (D14) | §0、§5-4 |
| — | `.kcgfont` 廃止 (ユーザー決定) | **反映**: 「焼いたキャッシュで初期充填」案を撤回、`boot_font` / `gen_font16.py` / `kcg_load_font` を撤去対象に | §5-4、§5-5 |

反論した点は無い — 10 件とも根拠の行を確認できた (`paging.c:796-812`、`exec.c:1932`、`kernel.c:425-434` と `:505/:558`、`readelf -r`、`fep.rs:326,388-391`、`ime.c:751-758`、`vk32_boot.c:83-92`)。

### 8-2. 往復 4 (v2 への指摘) → v3

| # | 指摘 | 対応 | 場所 |
|---|---|---|---|
| P2-3' | FEP 不在時も非ブロッキング API は生の入力を返す (`sys_keyboard.c:27` の `vz_kbhit` → `ime_trygetkey`、`ime.c:851`) | **反映**: `ime_trygetkey` = `kbd_trygetkey()` の生キー、`ime_trygetchar` = その文字。受入に CUI エディタを追加 | §5-6、§7 |
| P2-V86 | V86 は高位 lease 窓では届かない (`A800:0000` = 0xA8000)。通常アプリの不変条件と V86 セッションを分ける (`v86.c:93`、`v86_mem.c:73,99`) | **反映**: 不変条件は「V86 セッション外の CPL=3 AS」に限定、V86 はセッション中だけ低位に USER を立て終了時に復元 (既存機構) | §2-1、§2-2 |
| P2-map | 地図検査の「15〜16MB は NP」が PEGC のリニア窓 (sup+PCD) と矛盾 | **反映**: 台帳に MMIO 登録した範囲は sup+PCD、未登録は NP と期待値を分けた | §2-3 ⑥ |
| P2-2' | `--emit-relocs` は解決済み内部 PC32 も出す; 同梱域の余りを返す前に BSS・整列込みのサイズ (`vk32_boot.c:85` は `raw_size` だけ) | **反映**: モジュール内で完結する PC32 は検証して除外、未解決・未対応は拒否。ヘッダに `mem_size` / `bss_zero`、mkvk32 は BSS を含めて配置 | §4-2、§4-5 |
| P3-KCG | KCG ROM fallback のキャッシュ (64KB) が未計上 | **反映**: 行を足し、合計 3,619 / 3,447、余り +1,571 / +1,743 | §4-3、§5-5 |
| P3-(ii) | 受入アプリ (ii) が 2MB を超える | **反映**: 3 種とも内訳を 2,048KB ちょうどに (512KB スタックは維持) | §7 |

反論した点は無い。ユーザー判断として残した 5 点 (BB の (b)、PEGC 直描きの全画面限定、KCG ROM fallback、FEP/SQLite 省略構成、P8/P9 の別柱化) は Codex と一致し、**2026-09-30 にすべて決定** (D14・D19・D21)。v3 の最終案に対する Codex 往復 5 の判定は **Approve**。

### 8-3. 往復 6 (§3-5 池の運用規則 R1〜R6 への指摘、2026-09-30) → §3-5 改訂

| # | 指摘 | 対応 | 場所 |
|---|---|---|---|
| P1 | R1 (ISR は池を触らない) が現行の CTRL+STOP 経路 (IRQ1 スタブ → `ring3_abort_check` → `exec_exit` → `exec_teardown_app` → `pcm_reclaim` の `kfree`) と衝突。assert を入れると回復操作で止まる | **反映**: IRQ が行うのは要求 + 制御移譲だけ、回収は復帰点 (通常文脈) で。`ring3_abort_kill` を移譲 / 回収の 2 段に割る (T2)、T1 は数える検査。**補足 (反論ではない)**: 現行経路は割り込まれた文脈が CPL=3 のときだけなので台帳は中途ではなく、本当の害は IF=0 で tick が止まり `pcm_poll` の期限が来ないこと — これを根拠として書いた | R1、§3-5-2、§6 T1/T2 |
| P2-a | R2 (起動時に確保しきる) が §2-3 ⑧ (最小量から伸ばす) と矛盾 | **反映 (推奨案、ユーザー確認待ち)**: ⑧ の側に揃える。起動時は最小量、実行中は通常文脈の KAPI で伸長 (`brk` 相当) と大きな塊の `mem_map` / `mem_unmap`、失敗は NULL、unmap で即返却、ヒープは終了時 (trim はユーザー判断)、起動予約 (ユーザー判断) | R2、§2-3 ⑧ |
| P2-b | lease の初回に私有 PT の物理が要る (R3 と矛盾) | **反映**: AS 生成時に lease 窓の PT を 1 枚事前確保、超える分は通常文脈で 1 枚ずつ (R3-e)、取れなければ lease 全体を張らない (巻き戻し)。lease 台帳は固定配列 | R3-e |
| P2-c | 「私有 2MB 以内」が受入条件を一般の上限にしている | **反映**: 2MB は 8MB 機の受入条件、上限は機種の空き次第 (仮想は帯の枚数) | R2 2.、運用表 |
| P2-d | R5 のゼロ検査が gshell の永続 BB と衝突 (シェル終了も所有者回収を通る) | **反映**: owner を AS owner / 永続 owner の 2 種に分け、ゼロ検査は AS owner だけ、永続は R6 の予算検査で。移譲は boot → gshell の 1 回 | R5 |
| P3-a | R4 は固定 DMA プールの予約と利用時の割当 (`pcm_open` の `dma_pool_alloc`) を区別すべき | **反映**: 池からの連続確保は起動時・読み込み時だけ、プール内部の割当は利用時の通常文脈でよい。PCM の確保時点は変えない。最悪の並びを T1 の kselftest に | R4 |
| P3-b | モジュールの読み込み失敗の巻き戻しと取り外しの契約が無い。「R1〜R4 を見直す」では何が壊れるか不明 | **反映**: R7 (owner 単位の一括回収、装置状態は init が戻す、v3 は取り外さない、足すなら停止 → 参照解除 → 回収)。§3-5-3 に「台帳更新の非再入・owner の安定・lease/回収の順序」の 3 つを明記、先取り方針はプリエンプションと独立 | R7、§3-5-3 |
| — | 問題なしとされた点 (`kmalloc` は KHEAP 外へ出ない、shlib と BB 再利用の整合、V86 の連続確保は T7a で撤去、R6) | そのまま | R3、R6 |
| — | ユーザーの問い (同日): 実行中の追加確保と返し方、断片化 | R2 と §3-5-4 (物理連続が要るものの一覧と順序) | R2、§3-5-4 |

反論した点は無い (P1 の補足は根拠の言い換え)。**ユーザー判断が要る点**: R2 の採否 (伸長 + map/unmap)、大きな塊の閾値、ヒープの trim、起動予約の有無と量。

---

## 9. しないこと

- 本体のコードを削ること (診断文字列・kselftest の圧縮は対象外)。
- SHM の GUI 予約の移動 (SDK 定数 `GUI_SHM_OFFSET` は不変)。
- カーネルの仮想アドレス化 (高位カーネル) — v4 以降 (D20)。v3 では P2V / V2P の集約まで。
- アプリ私有サーフェス + WM 合成 ((c))、Video HAL / VESA2 的層 / SDL (P9)、音・入力の層 (P8) — 別の柱 (D19)。
- 互換層・旧バイナリの救済 (D7)。
- FEP の任意省略 (D21。MINIMAL 以外は常に読み込む)。
- FAT の 2 系統統合 (存在しなかった)。

---

## 10. 未確認の前提と、実装の段で測って決めるもの (U1〜U22)

| # | 前提 / 測るもの | どこで |
|---|---|---|
| U1 | §4-3 は見込み: SLACK の実値、owner/lease 台帳の実サイズ (16KB は推測)、モジュールの BSS・ページ切り上げ、shlib の再ビルド後の実ページ数 | T3 / T2 の受入 |
| U2 | VK32 の圧縮器での本体だけの大きさ (≈ 208KB は `lz4 -9`) | T4 の後 |
| U3 | MEMSYS5 の必要量 (8MB で 256KB に絞れるか) | T4 |
| U4 | OpenType のラスタ時間 (P100 / Ra266 / NP21/W / 386)、cold 1 画面の時間、ヒット率 | T7b |
| U5 | 16px の unhinted ラスタの見た目 (罠 §4-10) | T7b |
| U6 | FD 起動での字形ごとの読み (2 read/字) の遅さ (`.kcgfont` を捨てたので初期充填の救済は無い — KCG ROM の予備が受け皿) | T7b |
| U7 | V86 バッキング (636KB 連続) を撤去して低位を直接使えるか | T7a |
| U8 | 「アプリ帯 < 32MB」の暗黙の仮定が Codex 往復 2 ① の一覧で全部か | T2 |
| U9 | `MEM_APP_BAND_MAX_PDES = 64` + lease 窓で `struct addrspace` が収まるか | T2 |
| U10 | PCI の BAR が 0x80000000 以上に置かれる機種 | 台帳で MMIO 登録、2GB 超は窓の帯へ |
| U11 | `sysconfig` の SQLite 依存の経路 | T4 |
| U12 | フォントキャッシュを直読するユーザ側コードが無いこと | T7a |
| U13 | kstack 16KB のリング 3 導入後の再計測 | T3 |
| U14 | 0x90000 mailbox と V86 の低位直接使用 | T7a |
| U15 | gshell がシェル帯 1MB に BB 移譲 (所有だけ、物理は池) + フォント表 100 + キャッシュ 80 + ヒープ で収まるか | T2 / T7b |
| U16 | (c) の Paint ごとの WM コピーのコスト | サーフェス層の票 |
| U17 | PEGC の VRAM 読み戻し時間、8bpp 400 ラインの 2 面フリップ | P9 |
| U18 | (c') の再合成のちらつき | P9 |
| U19 | `paging_init` / 台帳をルートマウント (`kernel.c:434`) より前に動かせるか (今は逆。理由は未確認) — 動かせなければ同梱域は固定番地の静的予約 | T5b |
| U20 | CUI アプリ (`lconsole` 等) が TVRAM を直接書いているか → TVRAM lease の要否 | T2 |
| U21 | 共有グリフキャッシュ (gshell 所有、RO lease、GUI op で補充) の価値 — 私有 80KB × プロセス数 vs 契約の複雑さ | T7b の後 |
| U22 | モジュールの再配置表の大きさ (SQLite で十数 KB は推測) と、`memset` / `memcpy` をモジュール内に持つかスタブにするか | T4 |
| U23 | 起動後に読むモジュールが物理連続を取れないときの別案「カーネル側の仮想窓で多ページを束ねる」— 恒等写像 (D20) の外なので v3 では未確認。v3 のモジュールは全部起動時に読むので不要 (§3-5-4) | 後から読むモジュールを足す票 |

---

## 11. 経緯

### 11-1. 討論前の数字 (2026-09-23、Codex 往復 1 で訂正) — v2.1 の地図

| 帯 | 範囲 | 中身 | 余り |
|---|---|---|---|
| カーネル帯 | 0x100000〜0x1FFFFF (1MB) | 本体 (予算 596KB) → KHEAP 192KB → KAPI 4KB → SHM 224KB (末尾 64KB は GUI 予約) → 予約 | 134.2KiB (09-23)。**v2.1 時点は 583.0KB / 596KB、残り 13.0KB** ([02_memory.md §2-1](../../02_memory.md)) |
| SQLite 帯 | 0x200000〜0x2FFFFF (1MB) | SQLite 752KB → 代替スタック 128KB → 予約 (伸び代 44,960B) → DMA プール 64KB (0x2E8000) → 予約 12KB → ガード → カーネルスタック 16KB (0x2FC000) | |
| シェル帯 / shlib 帯 | 0x300000〜 / 0x400000〜 | 常駐シェル (2 ヒープ) / 共有ライブラリ (PDE 1 の私有 PT、`.text` は共有) | |

### 11-2. 討論前の票 (設計 v1 → v2、2026-09-23〜29) — 何が残り、どこで閉じるか

- **v1 の案 (旧 2-2 = 討論の案 A「番地の付け直し」)**: カーネル帯 0x100000〜0x2FFFFF (2MB)、SQLite 0x300000〜、シェル 0x400000〜、shlib 0x500000〜、プログラム 0x600000〜。Codex 往復 1 で「シェルを PDE 1 に置くと CPL=3 起動でシェルの写像が消える」ほか 9 件の反例 → **討論で不採用** (Fable・Codex 一致、§11-3)。
- **v2 の「決めるべきこと」B1〜B9**: B1 シェルの写像 (PDE 1 消去)、B2 pgalloc の配布域とシェルの分離、B3 固定 PD/PT の初期化順序 (PG=0 で作る → NP 化の後に張り直す → CR3 → PG)、B4 予算式 (浮動部分の上限を最初の固定領域の手前に)、B5 番地を焼く 5 経路 (`app_sys.ld` / `shlib.ld` / `mkshlib.py` / `stub.rs` / `--sqlite-addr`)、B6 NHD の移行単位 (一組で配備、旧シェル拒否)、B7 アプリ帯の開始・上端・枚数・物理配布域、B8 ローダの 508KiB 上限、B9 受入 (PDE 0 だけの検査では取り逃す)。→ **B1〜B7・B9 は T2/T3、B8 は T5b + T6b** (§6)。B3 の順序はそのまま T3 の実装規則。
- **旧 2-1 (ページ表を画像の外へ)**: master PD 1 + ブート PT 8 (+ デバイス窓 PT 1) = 40KB を固定の塊 (0x3F1000) へ → §2-1 の帯の表に取り込み済み (T3)。
- **旧 2-3 (着地済み、2026-09-23)**: `mem` / `heap` の地図の文言、`paging.c` の pd_raw / pt_raw を `aligned(4096)` に (整列の捨て 8KB を除去)、PCM のステージング 16KB を KHEAP へ。**これは v2.1 に入っている。**
- **当時の PM 判断「134KB の余裕で PCM と 82557 は入るので 2-2 は急がない」** は、v2.1 時点で残り 13.0KB になり前提が消えた (V3_PLAN_DRAFT §3-1 MD3)。

### 11-3. 3 者討論 (2026-09-29〜30) — ラウンドごとの要点

参加: ユーザー (決定者)・Fable 5.1 (`claude-fable-5-1`)・Codex (gpt-6-astra、`codex exec -s read-only`)。PM (Claude Code) は進行と取り次ぎ。基点 `feat/gui` 39a09b89、読み取りのみ。討論の原本はセッションの一時ファイル (`scratchpad/memdebate/`) で、要る内容はこの票に写した (原本は残らない)。

| ラウンド | 出典 (一時ファイル名) | 要点 |
|---|---|---|
| 議題 | `brief.md` | 材料 (02_memory §2-1、memmap.h、この票、DEVICE_RESERVATION、MEMORY_RAM_INTEGRATION、APP_BAND_PDE、K6、KSTACK_USER)、事実 (Ra266 64MB、PCI BAR 0x20000000 帯、NP21/W 17MB/8MB、15〜16MB のシステム空間、PEGC 窓 F00000h、Cirrus 0xFE000000、32 ビット限定)、意見書に書く 5 項目 |
| 1: 意見書 | `fable_1.md` / `codex_1.md` | **Fable**: 問題点 (PDE 0 満杯、アプリ帯が物理の地図に縛られる、top 予約、低位の時分割)、案 A「番地の付け直し」(旧 2-2) / **案 B「システムは恒等のまま、アプリの仮想帯だけを RAM の上へ」(推奨)** / 案 C「帯は動かさず育つ分はモジュールへ」。**Codex**: 「物理資源の管理と仮想アドレスの配置を分離し、per-app 物理割当を土台に段階移行」を推奨、案 A の反例、8MB は機能・要求量ごとに成立を判定、問い 3 つ (8MB の位置づけ・互換・快適の判定) |
| ユーザー回答 1 | `user_1.md` (09-29) | 8MB 機は GUI で 2MB のアプリまで動かせる状態が望ましい / バイナリ互換は捨ててよい (互換層は要らない、ソース互換は極力) / 快適の基準は当時重かったワード・エクセル級 + マルチメディアが Win95 (P100/32MB) より快適 |
| 2: 反論と合成 | `fable_2.md` / `codex_2.md` | **Codex** が Fable 案に寄せた合成案: 恒等 + アプリ帯 0x80000000 + 「物理資源の所有管理」「共通写像の更新規則」「仮想予約と物理消費の分離」を組み込む。8MB の成立は「2MB アプリ」の意味次第。**Fable** は Codex 案 B (非恒等の共通 supervisor 仮想領域) の反例 W1〜W5 (live-AS 規則・CR3 切替・監査対象の広さ・恒等の穴・常駐の数字) を挙げ、一致点 8 件 (§0 の D2〜D8 の元) と争点 3 件 (カーネル側の写像を恒等にするか / アプリ帯を動かす時期 / 常駐コアの数字) を整理、ユーザーに 7 点を問う |
| ユーザー回答 2 | `user_2.md` (09-29〜30) | 「2MB」は私有総量 / 8MB の GUI は PEGC も / `--cpl0` 廃止 (バックエンドは CPL=0 モジュール、当面静的、常駐シェルは例外を当面許可) / アプリ帯 0x80000000 / SQLite はカーネルの機能だが同一リンクにしない / 低位 640KB は V86 へ / フォントは OpenType・KCG 廃止を検討 / 同時使用はしない、快適の判定は前景 1 本 (承認) / 新しい問い「Unicode 表とフォントキャッシュを動的に置く方法、OpenType のグリフキャッシュだけでよいか」/ FEP も依存を断つ候補 / BB は planar だけでもよいか、BB の置き場をアプリか WM に / 後方互換は基本考えない / DirectX 的な抽象化層 / モジュール区分 (FDC はコア、スプラッシュはユーザーランド、表示はシリアルの前で十分) / `.kcgfont` 廃止 / 高位カーネルは v4 以降・v3 は P2V/V2P の集約だけ / 第 3 ラウンドの決め方 (D16) を承認 |
| 3: 最終案 v1 | `final_draft.md` (v1) | D1〜D16、帯の表、SQLite 分離、8MB の勘定、Unicode 組表 28KB、OpenType、低位解放、FEP、BB の持ち主 (a)(b)(c)(c')、票 T0〜T7、受入、U1〜U18 |
| Codex 往復 3 | `codex_3.md` | 判定「修正が必要」: P1 = lease の隔離 (共有 PT)・VRAM 常時 USER・ブート順 (ルート FS モジュール)。P2 = ローダの 8MB 証明・SQLite の再配置 (PC32 外部・`tick_count`)・FEP stub・OpenType の読み出し層・BB の先行確保・2MB の内訳。P3 = 数値の補正 → §8-1 |
| 最終案 v2 | `final_draft.md` (v2) | lease 窓 0xF0000000 (私有 PT)、VRAM 写像撤去、2 段階の起動順、区間表、emit-relocs + インポートスタブ、API 別 stub、ストリーミング層、票の分割 (T5a/b/c・T6b・T7a/b) |
| Codex 往復 4 | `codex_4.md` | 判定「P1 解消、P2 残」: FEP 非ブロッキング API、V86 は lease 窓で届かない、地図検査と PEGC 窓の矛盾、emit-relocs の内部 PC32 と `mem_size`、KCG キャッシュ未計上、受入 (ii) の超過 → §8-2 |
| 最終案 v3 | `final_draft.md` (v3) | §8-2 の 6 件を反映、D17〜D20 を追加、ユーザー判断 5 点 (BB (b) / PEGC 直描き限定 / KCG ROM 予備 / FEP・SQLite 省略構成 / P8・P9 別柱) を明示 |
| ユーザー判断 5 点 + 確定 | `user_2.md` 末尾 (09-30) | (1) BB は gshell 所有 + 全画面 lease の順で / (2) 承認 / (3) 漢字 ROM を予備に、承認 / (4) SQLite・FEP は常に読み込む、例外は MINIMAL だけ (D21) / (5) P8/P9 は別の柱。**目標の 2 段** (D22)、P6 の意図と P5 の追加は V3_PLAN_DRAFT へ |
| Codex 往復 5 | `codex_5.md` | **Approve** |
| ユーザー決定 + Codex 往復 6 | `codex_pool.md` (09-30) | §3-5 池の運用規則 R1〜R6 (ユーザー決定) → Codex「修正が必要」7 件 (R1 と CTRL+STOP の衝突、R2 と ⑧ の矛盾、lease の PT、2MB の上限化、R5 と永続 BB、R4 の予約と割当、モジュールの寿命と P6) → §3-5 改訂 (R1 の 2 段化、R2 は伸長側へ (推奨案)、R3-e、R5 の owner 2 種、R7、§3-5-3/-4) → §8-3 |
