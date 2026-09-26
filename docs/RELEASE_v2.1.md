# OS32 v2.1 リリースノート (下書き)

> 状態: **下書き**。タグ `v2.1` は、次の実機の回 ([CHECKLIST_2026-09-26](tasks/realhw/CHECKLIST_2026-09-26.md) の手順 1〜7) を通した後に
> feat/gui を main へ合流して付ける ([ROADMAP](ROADMAP.md) §0)。発行: PM (Claude Code `claude-opus-5-5`)、2026-09-26。
> 前の版: `v2.0` (2026-09-03、KernelAPI v39)。この版: KernelAPI **v68** (予定、`v86 -g` の着地後)。

v2.1 は **v3 へ分岐する前の区切り**。v2.0 (リング 3 ネイティブ) の上に GUI シェルを載せ、**実機 PC-9821Ra266 で FD 起動・CD からの
HDD インストール・HDD 起動まで**通した版。

## 1. 実機で動くようになったもの

| 分野 | 内容 |
|---|---|
| 起動 | FD (2HD 1232KB / 1.44MB の生イメージ) で起動、PIT はクロックを判定して分周 (2.4576MHz 系で 23% 速かったのを直した)、キーボード 8251 のコマンド語 (0x16)、ビープの止め方 (0035h を全体で書かない) |
| FD | シーク・回転の最悪値から時間上限、FRY、1MB 超への DMA (0439h)、トラック読みと 8 セクタの LRU |
| HDD | 標準の PC-98 区画表で OS32 の区画を作る `cdinst` / `install` (段 1・2)、他 OS の区画や壊れた表を消して入れる **ERASE**、HDD の IPL とローダ、イメージの CRC 検査 (VK32) と `ver` の `Commit` / `Image CRC` |
| CD | ATAPI の READ(10) を 16 セクタずつ、iso9660 のパス・ディレクトリのキャッシュと先読み (cdinst が約 10 分かかっていた件)、マスター/スレーブの両方から媒体のある装置を選ぶ、待ち上限を秒単位に (スピンアップでリセットしない、SRST 31 秒)、死んだバスはすぐ失敗 |
| シリアル | 115200bps までの整数分周、SerialFS (シリアル越しの `/host`) と `hsync --root` — **FD 起動から HDD を更新**できる |
| PCI | 列挙と `lspci -v` (82557 LAN、チューナーの識別) |
| 診断 | 起動ログ `/var/log/boot.log` (前回分は `.1`)、`kbdstat -w` (キーの make/break を 1 行ずつ)、`v86 -g` (実機の BIOS が 480 ラインで書く GDC の値を記録) |

## 2. GUI シェル (1.1〜1.3)

gshell (WM)、libos32gui.shlib、PEGC / Cirrus のバックエンド、ファイラー、エディタの GUI 版、FEP、設定レジストリ (settings.db)。
v2.1 で足したもの: **キーボードだけで GUI を操作する** (Windows 98 と同じ割り当て — CTRL+ESC、GRPH+TAB、GRPH+f･4、GRPH+SPACE の窓メニュー、
SHIFT+f･10、カナ ON のマウスキー。PC-98 の GRPH = Alt)。GUI 1.4 の残り (About、R2 計測) は v2.1 に含めない。

## 3. カーネル層で直した不具合 (主なもの)

- **GUI アプリが起動直後に消えていた** (2026-09-23〜26): KAPI の出力ポインタの検査が、アプリの syscall の中で走る WM (gshell) 自身の
  ポインタを弾いてアプリを kill していた。WM の文脈の深さで判定し、アプリが登録したバッファ (fd_redirect) は由来で必ず検査する。
- `ime_set_render` にアプリが関数表を渡すと、カーネルがそれを CPL=0 で呼び続けた — 常駐側だけに限る。
- VFS の FD の失効と ime_dict の開き直し、KAPI のデータ欄の固定 (v63)、SHM 帯とカーネルスタックの重なり、ext2 の書き込みの諸点。
- カナ・CAPS はロックキー (押し込んで make・外して break) として扱う。

## 4. 配布物

- **MINIMAL を「起動・HDD への導入・回復・残りの取得」に絞った** — フォントと一般コマンドは NORMAL へ、hsync を MINIMAL へ。
  FD 起動で赤い `FONT..NG` が出るのは正常 (テキスト画面は本体の CG、GUI は漢字 ROM で描く)。
- 2HD の起動 FD の空き 359KB (1.44MB は 568KB)。空きが 64KB を切ったら `make check` が落ちる。

## 5. 開発の道具

- `make check` の 3 段: `check-fast` (約 33 秒)・`check-changed` (変えた所だけ変異試験)・`check` (約 2〜3 分、見直し前は約 10 分)。
- NP21/W ai-debug フォーク: `/api/quit`・`/api/instance`・`/api/cd`・`/api/fdd` (有界な待ち)、`tools/np21w_ctl.py` (停止・起動・媒体の出し入れ)。
- CI (GitHub Actions) で本体をビルド、実機用の成果物は `tools/ci_fetch.sh`。

## 6. 分かっている制限 (v3 以降)

- 実機の PEGC 640x480 で桁がずれる (GUI を `gfxmode pc98` で使えば正しい)。`v86 -g` の NP21/W での記録では、BIOS が 480 ラインで
  PITCH 80・GDC 5MHz にするのに OS32 はしていない — 実機の記録で直し方を決める ([TASK_PEGC480_REALHW](tasks/realhw/TASK_PEGC480_REALHW.md))。
- `libos32ui` (microUI) のアプリにはマウスキーが届かない。
- CPL=3 のアプリのコールバック (`sys_ls`) は CPL=0 で同期的に呼ばれる (障害隔離のモデルで、敵対アプリの封じ込めは目標にしていない)。
