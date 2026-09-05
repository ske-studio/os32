## 第9部 外部プログラム実行 (exec)

OS32Xバイナリ形式の実行ファイルをext2から0x400000にロードし、
KernelAPIポインタを引数として実行する。

詳細は **[KAPI_SPEC.md](KAPI_SPEC.md)** を参照。

| 項目 | 値 |
|------|------|
| バイナリ形式 | OS32X (40バイトヘッダ + フラットバイナリ)。`tools/mkos32x.py` が付与 |
| ヘッダマジック | 0x4F533332 ('OS32') |
| ソースファイル | `exec/exec.c` / `exec/exec.h` / `exec/exec_heap.c` |
| KAPIラッパー | `kapi/kapi_*.c` (自動生成分 + 手動分)。版は [KAPI_SPEC.md](KAPI_SPEC.md) |
| KAPIテーブルアドレス | 動的算出 (KHEAP_BASE + KHEAP_SIZE) |
| ロードアドレス | `MEM_EXEC_LOAD_ADDR` = 0x400000 (共有ライブラリ帯域の導入で 0x500000 へ移す計画: [tasks/gui/TASK_K3](tasks/gui/TASK_K3_shared_lib_band.md)) |
| 特権レベル | **既定で CPL=3** (v2 M1〜M3, 2026-09-03)。例外は shell (CPL=0 常駐) と `OS32X_FLAG_FORCE_CPL0` (`mkos32x --cpl0`) |
| アドレス空間 | プログラムごとに PD。カーネル帯 0x100000〜0x3FFFFF は全 PD 共有・非 USER、SHM / VRAM は USER |
| ヒープ | [本体][newlib sbrk (最低 256KB)][ガード][exec_heap] を**ロード時に動的に決める** (固定 1MB 上限は 2026-09-04 に撤廃)。exec_heap の大きさは OS32X ヘッダ `heap_size` (`mkos32x --heap`) があればそれ、0 なら空きを sbrk と折半。実行中の拡張は無い |
| ネスト実行 | 最大 4 段 (`MAX_EXEC_NEST`)。Level 0 = カーネル、1 = シェル、2+ = アプリ。**子が終了すると親に戻る** (親の exec_heap は `exec_heap_restore_state()` で復元) |
| 資源の所有者 | FD / リダイレクト / パイプは `res_owner_get()` (= ネスト段) でタグ付け、終了段の分だけ回収 ([10 §10-9](10_notes.md)) |
| 不正ポインタ | ディスパッチャがアプリ帯 / SHM / VRAM の範囲で早期検証。検証しきれないものは「ring3 syscall 実行中フォールトガード」が捕捉し、**アプリだけ kill** (`fault_kill_count`)。設計: [tasks/v2/](tasks/v2/PLAN.md) |
| プログラム専用スタック | CPL=3: 物理 0x7C0000〜0x7FFFFF 固定 (256KB) + ガード 0x7BF000。CPL=0: mem_end 付近 |
| 呼び出し規約 | カーネル側 GCC (System V) + `__cdecl` ラッパー、外部プログラム System V i386 ABI |

### 実行方式

```bash
> ./program.bin     # 直接実行
> program           # 未知コマンド → PATH 探索で *.bin を補完
> exec program.bin  # 明示的 exec
```

`api->exec_run()` で別プログラムを起動すると子として走り、終了で呼び出し元に戻る
(`execve` 置換ではない。2026-09-05 訂正)。戻り値は下表のステータス。

### `exec_run` の構造と分割壁

`exec_run()` は約 280 行だが**関数分割はしない**。内部で `exec_setjmp()` / `exec_longjmp()`
によるコルーチン的制御を持ち、setjmp 後のブロック (ローカル変数の寿命、`saved_esp` の
インライン asm、`exec_nest_level` の増減) は物理的に別関数へ出せない。

1. setjmp 後のブロック (argv 構築 + asm 実行) は分離禁止
2. 拡張は setjmp 前のブロック (パス解決・ファイル読込・ヘッダ検証) にだけ足す
3. 300 行を超えたら、setjmp 前を `exec_load_binary()` として切り出すことを検討する

(構造分析レポート `exec_run_analysis.md` (2026-04-16) は削除済み。必要なら git 履歴。)

### 実行ステータスコード (`exec_status_t`)

`exec_run` 関数は実行結果として `exec_status_t` (定義: `os32_kapi_shared.h`) を返す。

| ステータス | 値 | 説明 |
|---------|---------|------|
| `EXEC_SUCCESS` | `0` | 正常終了 |
| `EXEC_ERR_GENERAL` | `-1` | 一般的なエラー（ロード失敗等） |
| `EXEC_ERR_FAULT` | `-2` | 例外やフォールトによるプロセスの異常終了 |
| `EXEC_ERR_NOT_FOUND` | `-3` | 指定されたOS32Xバイナリが見つからない |
| `EXEC_ERR_NOMEM` | `-4` | メモリサイズ超過、ページング領域確保失敗等のメモリ不足 |
| `EXEC_ERR_INVALID` | `-5` | ヘッダマジック不一致やバージョン非互換等の不正なバイナリ |

### プログラムの一覧

表はここに持たない (2026-09-05 に撤去。ゲームライブラリの所在など古くなっていた)。
正典は各層の配備マニフェスト `userland/deploy.yaml` / `apps/deploy.yaml` / `game/deploy.yaml`
(`tools/deploy_manifests.py` がマージ) と、コマンドは [07_shell.md §7-1](07_shell.md)。
ライブラリの設計書は `docs/tasks/lib*/`、ゲームエンジン 11 ライブラリは
`ske-studio/os32-game` の `docs/`。
