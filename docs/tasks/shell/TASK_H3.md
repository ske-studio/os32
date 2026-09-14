# TASK_H3 — mtime の取得と保存 (`sys_set_mtime` + VfsOps フック)

- 発行: PM (Claude Code / Fable 5.1) 2026-09-15
- 正典: [`HSYNC_IMPROVEMENT_PLAN.md`](HSYNC_IMPROVEMENT_PLAN.md) §5 (取得・保存)、§8 の **H3** 行
- 前提: [`TASK_H1.md`](TASK_H1.md) の着地 (内容比較と CRC 検証)。H2 とは独立
- 状態: 未着手

## 1. なぜ H1 と分けて、しかし先送りにしないか

現状は **時刻を読めるのに設定できない**という非対称になっている。

| 経路 | 現状 |
|---|---|
| `OS32_Stat.st_mtime` | 定義済み (`sdk/include/os32/os32_kapi_shared.h:464`) |
| ext2 の読み | 実装済み (`fs/ext2_vfs.c:277`) |
| FAT の読み | 実装済み (`fs/fatfs_vfs.c:325`) |
| `sys_stat` の CPL=3 公開 | 実装済み (KAPI スロット 8) |
| **HostDrv の読み** | **`hdrv_stat()` が `st_mtime` を埋めていない** (`fs/hostdrvfs.c:635`) |
| **VfsOps の設定フック** | **無い** |
| **KAPI の設定口** | **無い** |

ext2 は inode に `mtime` を保持し、書き込みのたびに現在時刻で更新している (`fs/ext2_file.c:183`)。
値は持っているのに外から与えられないだけで、能力が欠けているわけではない。

KAPI は追記専用 ([ABI2]) なので、末尾へのスロット追加は既存の ABI を壊さない。
カーネルが持つ能力を CPL=3 へ渡すのに `__cdecl` ラッパを足すのは本プロジェクトの通常手順であり ([C3])、
**スロットを足さないことに価値は無い**。H1 と分ける理由は設計思想ではなく次の 2 つだけである。

1. [ABI3] により版を上げて `make clean` → `make all` と `make external` (`apps/` / `game/` の再ビルド) が要る。
   H1 の着地をこのゲートに縛らない。
2. FILETIME の変換境界 (1970 年より前・未提供・範囲外) と非対応 FS の扱いが、比較ロジックとは別種の検証を要する。

hsync のコピー元は HostDrv なので、**§3 の取得側を直さない限り時刻は一切伝播しない**。
H3 は取得と保存の両方で 1 つの機能になる。

## 2. 範囲

計画 §8 の H3 行: 「HostDrv 時刻変換、mtime 設定 API、時刻のみの同期」。
対象は `fs/hostdrvfs.c`、`VfsOps`、ext2、`sdk/kapi.json`、`hsync`。
`atime` / `ctime` は同一判定に使わない。`OS32_Stat` の拡大は本票に含めない (時刻欄は既にある)。

## 3. 取得 (計画 §5.1)

- `hdrv_stat()` が `LastWriteTime` を `st_mtime` へ返す。NT 系 FILETIME は **1601-01-01 UTC 起点・100ns 単位**。
  Unix 秒へ変換し秒未満を切り捨てる。変換は **64bit 中間値**で行い、1970 年より前・未提供値・変換不能・
  `os_time_t` の範囲外を検査して回り込みを防ぐ。**UTC に JST の 9 時間を加算しない** (ローカル時刻化は表示側)。
- Basic / Standard 問い合わせの失敗を成功にしない。ホスト open のエラーを一律 NOTFOUND に畳まず、
  実際の不存在と I/O 等を区別する (H1 の stat 是正と同じ箇所なので**着地順に注意**)。
- 取得した Basic 情報は、次の問い合わせで共有バッファが上書きされる前に値として退避する。
- 現行 ABI に時刻の有効性ビットが無いため、hsync は **`mtime = 0` を「同一判定の根拠に使えない」と保守的に扱う**。
  Unix epoch そのものと不明の区別は将来の拡張 stat で定義する (計画 §10 の未確定 (2))。

## 4. 保存 (計画 §5.2)

- 仮称 `sys_set_mtime(path, mtime)` を `sdk/kapi.json` の**末尾**に追加し、VfsOps に**任意実装のフック**を足す。
  **現段階でスロット番号を予約しない**。版は実装時に [`KAPI_SPEC.md`](../../KAPI_SPEC.md) と調停する (現行 51)。
- **ext2 のみ先行実装**し、非対応 FS は `NOSYS` を返す。読取専用・不正時刻・パス／ポインタ不正は明示エラー。
- **データを書き終えてから**コピー元 mtime を設定する。宛先 `ctime` はゲスト側の変更時刻とする。
  H2 の rename も `ctime` を変え得るので適用順を固定する。
- 内容が同じで時刻だけ違うなら**本体を書き直さず mtime だけ更新**する (`metadata_updated`)。
- 元の mtime が不明なら時刻保存を省略したことを表示し、内容同期は続行する。
  有効な時刻の保存を試みて失敗したら **`metadata_failed` + 非ゼロ終了**。
  内容コピーの成功だけで全成功と表示しない。
- H1 で導入した集計に `metadata_updated` を加える (計画 §7.2 の 6 区分が揃う)。

## 5. ABI 手順 ([ABI1]〜[ABI3])

`sdk/kapi.json` が唯一の真実。生成物を手で編集しない。版を上げ、`make clean` → `make all` →
`make external` で `apps/` と `game/` を再ビルドし、全生成ファイルへの影響を確認する。
CPL=3 からのポインタ検証を含める。

## 6. 受入 (計画 §9 のうち H3 該当)

| ID | 反例・操作 | 期待結果 |
|---|---|---|
| A03 | 内容同一で mtime だけ相違 | 本体コピーなし、時刻のみ同期 (`metadata_updated`) |
| A04 | 元の mtime が古いが内容が違う | 通常モードは同期する |
| A16 | FILETIME 既知日時、秒未満、1970 年前、未提供、最大値超過 | UTC 秒への変換／不明扱いが正しく、wrap しない |
| 追加 | 非対応 FS への設定 | `NOSYS`。内容同期は継続し、省略を表示 |
| 追加 | 設定失敗 | `metadata_failed` + 非ゼロ終了 |
| 追加 | `mtime = 0` の元ファイル | 同一判定の根拠に使わない (内容比較で決める) |

ホスト試験を `tools/tests/` に置き `build/sdk.mk` に登録、RED→GREEN を `tools/tests/h3_tdd.md` に記録する。

## 7. 制約

[C1] C89、[C2] カーネル側は kstr*、[C3] `__cdecl` ラッパ、[C4] 定数 3 層、[ABI1]〜[ABI3]。
コーダーは git commit/push・配備・エミュレータ操作・`*.ini`/`.env`・`make all`/`check`/`deploy*` を行わない。
