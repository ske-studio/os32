# ============================================================================
#  sdk.mk — OS32 SDK の staging
#
#  外部プログラムが OS32 向けにビルドするために必要なものだけを
#  build/sdk/ に固める。ここに入っていないものに依存するプログラムは、
#  リポジトリを分割したときにビルドできなくなる。
# ============================================================================

SDK_OUT = build/sdk

# 公開ヘッダを SDK に載せるプラットフォームライブラリ。
# rt/ は "rt/dbgserial.h" 形式で引くので別扱い (下のルール参照)。
SDK_LIB_HEADER_DIRS = math gfx db ui input asset snd tilemap md filer mgx save ecs cfg

# kapi.json の version が KAPI バージョンの唯一の情報源。
# ヘッダ・Rust バインディング・ドキュメントはすべてここから導出する。
KAPI_VERSION := $(shell python3 -c "import json;print(json.load(open('sdk/kapi.json'))['version'])")

sdk: $(ALL_LIB_ARCHIVES) $(CRT0_OBJ) $(DBG_OBJ) $(SDK_KAPI_HDR)
	@rm -rf $(SDK_OUT)
	@mkdir -p $(SDK_OUT)/include/os32 $(SDK_OUT)/lib $(SDK_OUT)/crt \
	          $(SDK_OUT)/link $(SDK_OUT)/bin $(SDK_OUT)/rust
	cp sdk/include/os32/*.h              $(SDK_OUT)/include/os32/
	@# プラットフォームライブラリの公開ヘッダ。アーカイブだけ配っても
	@# ヘッダが無ければ使えない。internal と付くものは実装内部なので除く。
	@for d in $(SDK_LIB_HEADER_DIRS); do \
	    for h in userland/lib/$$d/*.h; do \
	        case "$$h" in *_internal.h) continue;; esac; \
	        cp "$$h" $(SDK_OUT)/include/ || exit 1; \
	    done; \
	done
	@mkdir -p $(SDK_OUT)/include/rt
	cp userland/lib/rt/*.h                $(SDK_OUT)/include/rt/
	@# 共有 C の公開ヘッダ。実装は libos32gfx.a に入っている (utf8_prog.o)。
	cp lib/utf8.h                         $(SDK_OUT)/include/
	cp $(LIBDIR)/*.a                     $(SDK_OUT)/lib/
	cp $(CRT0_OBJ) $(DBG_OBJ)            $(SDK_OUT)/crt/
	cp sdk/link/*.ld                     $(SDK_OUT)/link/
	cp sdk/mkos32x.py                    $(SDK_OUT)/bin/
	cp sdk/rust/i686-os32-none.json      $(SDK_OUT)/rust/
	cp -r sdk/rust/os32api               $(SDK_OUT)/rust/
	@rm -rf $(SDK_OUT)/rust/os32api/target
	@# 動くサンプル。SDK だけでビルドできることの実証も兼ねる。
	@mkdir -p $(SDK_OUT)/example
	cp -r sdk/example/hello              $(SDK_OUT)/example/
	@rm -f $(SDK_OUT)/example/hello/*.o $(SDK_OUT)/example/hello/*.elf \
	       $(SDK_OUT)/example/hello/*.raw $(SDK_OUT)/example/hello/*.bin
	cp sdk/README.md                     $(SDK_OUT)/
	@echo $(KAPI_VERSION) > $(SDK_OUT)/KAPI_VERSION
	@echo "=== OS32 SDK $(SDK_OUT) (KAPI v$(KAPI_VERSION)) ==="
	@echo "  include       $$(ls $(SDK_OUT)/include/*.h | wc -l) lib headers"
	@echo "  include/os32  $$(ls $(SDK_OUT)/include/os32 | wc -l) contract headers"
	@echo "  include/rt    $$(ls $(SDK_OUT)/include/rt | wc -l) runtime headers"
	@echo "  lib           $$(ls $(SDK_OUT)/lib | wc -l) archives"
	@echo "  crt           $$(ls $(SDK_OUT)/crt | wc -l) objects"
	@echo "  link          $$(ls $(SDK_OUT)/link | wc -l) linker scripts"
	@echo "  example       hello (SDK のみでビルドできる最小例)"

# ----------------------------------------------------------------------------
#  配布用 tarball
#
#  サードパーティが OS のソースを持たずにアプリを作れるようにするための
#  成果物。KAPI バージョンをファイル名に入れてあるので、どの OS 世代向けに
#  ビルドされたアプリかが一目で分かる。
#
#  展開して OS32_SDK を向けるだけで使える:
#    tar xzf os32-sdk-39.tar.gz
#    make -C myapp OS32_SDK=$(pwd)/os32-sdk-39
# ----------------------------------------------------------------------------
SDK_DIST_NAME = os32-sdk-$(KAPI_VERSION)
SDK_DIST_DIR  = build/dist

sdk-dist: sdk
	@rm -rf $(SDK_DIST_DIR)/$(SDK_DIST_NAME)
	@mkdir -p $(SDK_DIST_DIR)
	cp -r $(SDK_OUT) $(SDK_DIST_DIR)/$(SDK_DIST_NAME)
	@python3 tools/gen_sdk_manifest.py $(SDK_DIST_DIR)/$(SDK_DIST_NAME)
	tar czf $(SDK_DIST_DIR)/$(SDK_DIST_NAME).tar.gz \
	        -C $(SDK_DIST_DIR) $(SDK_DIST_NAME)
	@rm -rf $(SDK_DIST_DIR)/$(SDK_DIST_NAME)
	@echo "=== $(SDK_DIST_DIR)/$(SDK_DIST_NAME).tar.gz "\
	      "($$(stat -c%s $(SDK_DIST_DIR)/$(SDK_DIST_NAME).tar.gz) bytes) ==="

# 手書きされた KAPI バージョンが kapi.json とずれていないか検査する。
# ずれていると「どれが本当の版か」が分からなくなる。
check-kapi-version:
	@python3 tools/check_kapi_version.py

# 配備マニフェストと app.conf の参照先を検査する (要 make all)
check-manifests:
	@python3 tools/check_manifests.py

# プロジェクト制約 (docs/CONSTRAINTS.md) と、それを参照する CLAUDE.md /
# SOUL.md のずれを検査する。ID での照合なので文言は場所ごとに変えてよい。
check-constraints:
	@python3 tools/check_constraints.py

# ユーザランドの特権命令検査 (リング3 準備)。既定は警告のみ (exit 0) で
# green ビルドを壊さない。リング3 導入後に --strict でゲートする。
check-privileged:
	@python3 tools/check_privileged.py

# カーネル側 C ソースの hlt / cli / sti 直書き検査 (移植性の準備、順序 2)。
# これらは include/io.h の原始命令 (_halt / _idle / _stop / _enable /
# _disable / irq_save / irq_restore) 経由で使い、arch 差し替えの境界を
# io.h 1 枚に閉じ込める。切り出せない箇所は asm の直前に ARCH-ASM-OK と
# 理由を書く。
check-arch-asm:
	@python3 tools/check_arch_asm.py

# 外部形式 (LE) の直アクセス検査 (移植性の準備、順序 4-a)。媒体・書庫の上の
# バイト列は include/endian_le.h の le16_rd / le16_wr / le32_rd / le32_wr を
# 通す。`*(u32 *)&buf[off]` は「x86 は LE」「x86 は非アラインを許す」の 2 つに
# 同時に寄りかかる書き方で、ARM では落ち、BE では値が化ける。
# 文字列検査 (コンパイラ不要) と -Wcast-align=strict の 2 段で見る。
check-le-access:
	@python3 tools/check_le_access.py

# GUI 共有プロトコル (os32_gui_shared.h ⇄ proto.rs) の定数・構造体の照合。
# GUI v1.2 の G0 (契約凍結) のゲート。PM 所有 (docs/tasks/gui/v12/TASKS.md §5)。
check-gui-proto:
	@python3 tools/check_gui_proto.py

# 独立端末モデルのホスト試験。guest用Cargo設定を避けるためrootから実行。
# ゲストクロスリンク・描画・CUI統合の検証ではない。
check-term-model:
	cargo test --manifest-path userland/libos32term/Cargo.toml --target x86_64-unknown-linux-gnu --offline
	cargo check --manifest-path userland/libos32term/Cargo.toml --lib --target x86_64-unknown-linux-gnu --offline

# 純粋描画アダプタ。人工glyphによるホスト試験であり実ROM描画の検証ではない。
check-term-render:
	cargo test --manifest-path userland/libos32term_render/Cargo.toml --target x86_64-unknown-linux-gnu --offline
	cargo check --manifest-path userland/libos32term_render/Cargo.toml --lib --target x86_64-unknown-linux-gnu --offline

# T5aの純粋状態・座標・所有権試験。guest.rsの実行は含まない。
check-t5a-host:
	cargo test --manifest-path userland/rust/t5a_display/host_tests/Cargo.toml --target x86_64-unknown-linux-gnu --offline

# K5a (4 アプリ、契約 T2a) の設計をホストの純粋状態機械で固定したもの。カーネル実装の
# 正しさは何も言わない (実装は K5b)。docs/tasks/gui/v13/TASK_K5_multiapp.md §設計。
check-multiapp-model-host:
	python3 -B tools/tests/test_multiapp_model.py
	python3 -B tools/tests/test_multiapp_impl.py
	python3 -B tools/tests/test_owner_reclaim.py

check-memory-host:
	python3 -B tools/tests/test_physmem.py
	python3 -B tools/tests/test_paging_bounds.py
	python3 -B tools/tests/test_app_band_pde.py
	python3 -B tools/tests/test_pgalloc_model.py
	python3 -B tools/tests/test_pgalloc_range.py
	python3 -B tools/tests/test_highram_stage.py
	python3 -B tools/tests/test_memory_boot.py
	python3 -B tools/tests/test_device_reservation.py
	python3 -B tools/tests/test_sbrk_tier.py

# 記録: tools/tests/memmap_tdd.md (票 docs/tasks/memory/TASK_KSTACK_USER.md)
#
#   check-memmap       実ツリーの地図を検査する。帯どうしの重なり・範囲の逆転・
#                      カーネル本体の予算超過・memmap.h の値を写している場所
#                      (build/os32.ld / kernel/kentry.asm / SDK) のずれ・
#                      docs/02_memory.md の鮮度。**kernel.map が要る**ので
#                      カーネルを組んでいないと 2 で止まる。
#   check-memmap-host  合成した地図で道具と自己診断の挙動を見る。実ツリーの
#                      番地に依存しないので、番地を動かしても腐らない。
# 2026-09-17 (決裁 D1/D2) の配置で両方とも緑になったので check: の列に入れた。
check-memmap-host:
	python3 -B tools/tests/test_memmap_gen.py
	python3 -B tools/tests/test_memmap_boot.py

check-memmap:
	python3 tools/gen_memmap.py --check

check-boot-splash-host:
	python3 -B tools/tests/test_boot_splash_native.py

check-tools-host:
	python3 -B -m unittest discover -s tools/tests -p 'test_np21w_*.py'
	python3 -B tools/tests/test_nhd_deploy_failure.py
	python3 -B tools/tests/test_filer_normalize.py
	python3 -B tools/tests/test_filer_copy_abort.py
	python3 -B tools/tests/test_gui_button_dispatch.py
	PYTHONPATH=. python3 -B tools/tests/test_emu_playbook.py
	python3 -B tools/tests/test_mk_settings_db.py
	python3 -B tools/tests/test_mk_blank_nhd.py
	python3 -B tools/tests/test_stat_cmd.py
	python3 -B tools/tests/test_tar_cmd.py

check-gshell-host:
	python3 userland/gshell/host/integration.py

check-db-owned-host:
	python3 -B -m unittest discover -s tools/tests -p 'test_kapi_db_owned.py'

check-vfs-fd-sqlite-host:
	python3 tools/tests/test_vfs_fd_sqlite.py

# fs/vfs.c + fs/ext2_vfs.c の mount 経路。fd0 が hd0 に化けて同じ
# パーティションを二重マウントする回帰 (2026-09-10) を止める。
check-vfs-mount-dev-host:
	python3 -B tools/tests/test_vfs_mount_dev.py
	python3 -B tools/tests/test_ext2_read_bound.py
	python3 -B tools/tests/test_ext2_write_io.py
	python3 -B tools/tests/test_fatfs_stat.py

check-sqlite-groups-host:
	python3 tools/tests/test_sqlite_groups.py

# kernel/con_sink.c のリング (票 K6C)。実物のソースをホスト ILP32 で走らせ、
# 同じソースが i386-elf-gcc -Werror でも通ることを見る。記録は
# tools/tests/con_sink_tdd.md。
check-con-sink-host:
	python3 -B tools/tests/test_con_sink.py

# kernel/kbd_inject.c の 256B リング (票 K7)。実物のソースを kernel/con_sink.c と
# 同じ翻訳単位で走らせ (注入の権限は con_sink の読み手 1 本)、同じソースが
# i386-elf-gcc -Werror でも通ることを見る。記録は tools/tests/k7_tdd.md。
check-kbd-inject-host:
	python3 -B tools/tests/test_kbd_inject.py

# exec/launch.c の起動要求表 (票 T9 D3)。実物のソースを exec/appslot.c と同じ
# 翻訳単位で走らせ (要求者・子の所有・token の照合は AppSlot を引く)、同じ
# ソースが i386-elf-gcc -Werror でも通ることを見る。記録は tools/tests/t9_tdd.md。
check-launch-host:
	python3 -B tools/tests/test_launch.py

# exec/ring3_str.c — KAPI が CPL=3 へ **返す** 文字列の置き場 (票 T9 §12 R1)。
# カーネル帯には USER ビットが無いので、sys_getcwd がそのまま返すと CPL=3 の
# 呼び手が #PF で畳まれる。写し先 (トランポリンページの空き) の番地の式と
# 経路の分岐をホストで踏む。記録は tools/tests/t9_tdd.md。
check-ring3-str-host:
	python3 -B tools/tests/test_ring3_str.py

# userland/shell/sh_launch.inc の起動待ち (票 T9 D3a)。実物のソースを
# tools/tests/sh_launch_host.c がそのまま #include し、KernelAPI の
# launch_req / launch_poll / sys_yield / kprintf を差し替えて DONE / FAILED /
# STALE / FULL の 4 経路と「待ちの間 kbd_* / ime_* を呼ばない」を見る。
# 同じソースが i386-elf-gcc -Werror でも通ることも別に見る。記録は t9_tdd.md。
check-sh-launch-host:
	python3 -B tools/tests/test_sh_launch.py

# userland/shell/sh_redraw.inc の行再描画 (実装レビュー blocker 1 — GUI 中は
# コンソール座標が動かない) と userland/shell/cmd_script.c の source 中 exit
# (blocker 2 / D2(d))。どちらも実物のソースを tools/tests/sh_shell_host.c が
# そのまま #include する。記録は tools/tests/t9_tdd.md。
check-sh-shell-host:
	python3 -B tools/tests/test_sh_shell.py

# シェルが入力を黙って切り詰める経路 (票 docs/tasks/shell/TASK_SH_TRUNCATION.md)。
# tools/tests/sh_truncation_host.c が userland/shell/main.c を丸ごと #include し、
# **登録表も execute_command も実物のまま**回す (sh_shell_host.c は
# execute_command をスタブにしているのでこの経路を試験できない)。
# 段 2 の時点で T1 (`if` の比較) と §2-1 (スクリプト中で断ったら打ち切る) が
# 入っている。否定側は `--mutate` (印を立てない / 消し忘れる / 対話でも
# 打ち切る 版などが RED になる)。記録は tools/tests/sh_truncation_tdd.md。
check-sh-truncation-host:
	python3 -B tools/tests/test_sh_truncation.py

# 終了コードの配線と `$?` (票 docs/tasks/shell/TASK_EXIT_STATUS.md)。
# tools/tests/sh_status_host.c が userland/shell/main.c を丸ごと #include し、
# **登録表も execute_command も実物のまま**回す (受入 R2)。同じ 1 本を
# **2 通り** — 常駐 (exec_run + exec_last_result、KAPI v55) と
# -DSHELL_AS_APP (要求表経由) — にコンパイルして両方走らせる。種別ごとの
# 写像はビルドで実装が違うので、片方だけでは配線を見たことにならない。
# 否定側は `--mutate` (種別を値から作る / PATH 走査を値で止める /
# `exit` の値を捨てる / `set -e` を拾わない 版などが RED になる)。
# 記録は tools/tests/sh_status_tdd.md。
check-sh-status-host:
	python3 -B tools/tests/test_sh_status.py --mutate

# 通常配備が /etc/settings.db* を作らない・上書きしない・消さない (票 S0-D / D0)。
# temp dir + mock だけで走り、sudo / mount / 実配備は試験側が遮断する。記録は tools/tests/s0_tdd.md 節 D。
check-settings-protect-host:
	python3 -B tools/tests/test_deploy_protect.py
	python3 -B tools/tests/test_hsync_protect.py

# hsync の同サイズ更新の検出 (票 H1、docs/tasks/shell/HSYNC_IMPROVEMENT_PLAN.md
# §9 の A01〜A13)。実物の userland/system/hsync.c を #include し、KernelAPI だけを
# オンメモリの贋 FS に差し替えて回す。fs/hostdrv_stat_rules.inc (HostDrv の stat
# 失敗の是正) と CRC ストリーム核の既知ベクトルも同じ翻訳単位で見る。
# 記録は tools/tests/h1_tdd.md。
check-hsync-h1-host:
	python3 -B tools/tests/test_hsync_h1.py --target

# hsync の mtime 取得・保存と日時の前置判定 (票 H3、docs/tasks/shell/TASK_H3.md
# §6 / §8)。H1 と同じく実物の userland/system/hsync.c を #include し、贋 FS に
# **ノードごとの mtime** と sys_set_mtime (成功 / NOSYS / I/O 失敗) を持たせて回す。
# FILETIME (1601 起点・100ns) -> Unix 秒の境界 (A16) は fs/hostdrv_stat_rules.inc の
# 純関数を直接叩き、vfs_set_mtime の NOSYS 振り分けは実物の fs/vfs.c で見る。
# --mutate は否定側 (日時が不明なのに省略する版などで落ちることの確認)。
# 記録は tools/tests/h3_tdd.md。
check-hsync-h3-host:
	python3 -B tools/tests/test_hsync_h3.py --target --mutate

# 排他的作成 O_EXCL (票 H2 §2-1、KAPI v53)。実物の fs/vfs.c + fs/vfs_fd.c を
# #include し、create_excl を**持つ / 持たない**合成 VfsOps で、既存 (ファイル /
# ディレクトリ) が EXIST、判定不能がその負値、O_EXCL 単独が INVAL、非対応 FS が
# NOSYS になることを見る。**非対応の判定が種別検査より先**であることは
# get_file_size / list_dir の呼び出し回数 0 で押さえる。
# --mutate は否定側 (読めなかったを無いと読む版 / 非対応の判定を後ろへ動かした版)。
# 記録は tools/tests/vfs_excl_tdd.md。
check-vfs-excl-host:
	python3 -B tools/tests/test_vfs_excl.py --target --mutate

# 配備マニフェストと世代の確認 (票 H4、docs/tasks/shell/TASK_H4.md §2-1〜§2-3)。
# **読む側**は H1 / H2 / H3 と同じく実物の userland/system/hsync.c を #include し、
# 贋 FS の /host/.deploy/manifest.txt に票が挙げた壊し方を注入して回す。
# 名札が無い配備元が今までどおり動くこと、壊れた名札を捨てること、断るのが
# `--expect-build` かつ全体同期のときだけであること、**名札が読めないことを
# 「一致」と扱わない**こと、名札を信じて内容比較を省かないことを見る。
# **書く側** (tools/hostdrv_deploy.py) は一時ディレクトリだけで回し、全件成功の
# 後にだけ書くこと・失敗したら既にある名札を消すこと・一時ファイル + 置き換え・
# --no-manifest を見る。両層が同じ名札を指していることは静的に突き合わせる。
# --mutate は否定側 (壊れた名札を一致と扱う版 / 名札の CRC で比較を省く版など)。
# 記録は tools/tests/h4_manifest_tdd.md。
check-h4-manifest-host:
	python3 -B tools/tests/test_h4_manifest.py --target --mutate
	python3 -B tools/tests/test_hostdrv_manifest.py --mutate

# hsync の置換安全化 (票 H2、docs/tasks/shell/TASK_H2.md §2-3 / §2-4)。H1 / H3 と
# 同じく実物の userland/system/hsync.c を #include し、贋 FS に O_EXCL /
# sys_rename (公開の前に失敗 / 公開の後に失敗 / 判定不能) / ROFS / st_nlink /
# api->version を持たせて回す。公開の判定が **st_ino** であること、NOSYS で
# 直接上書きへ落ちないこと、予約名 .hs~ の掃除が st_nlink を見ないこと、
# 保護が予約より優先されること、KAPI v53 未満を既定で断ることを見る。
# 記録は tools/tests/hsync_h2_tdd.md。
check-hsync-h2-host:
	python3 -B tools/tests/test_hsync_h2.py --target --mutate

# hdrv_list_dir の列挙ループ (票 H1 の「I/O 失敗を成功にしない」/ 対象
# 「HostDrv のエラー処理」)。実物の fs/hostdrv_list_rules.inc を #include し、
# hostdrv_query_dir に当たる 1 件取得だけを贋物にして、(a) 途中で負値 /
# (b) 件数上限での打ち切り が VFS_OK で返らないことを見る。
# 記録は tools/tests/h1_tdd.md。
check-hostdrv-list-host:
	python3 -B tools/tests/test_hostdrv_list.py --target

# シェルの種別判定と cp -r の宛先階層 (票 H1 / 往復 3 の B5)。実物の
# cmd_fs_shared.c + cmd_file.c を #include し、sys_stat は正しいまま sys_ls だけを
# FULL / IO にして、列挙のエラーが「ディレクトリでない」に化けないことを見る。
check-fs-kind-host:
	python3 -B tools/tests/test_fs_kind.py --target

# 種別が「分からない」とき cp / mv / rm が断る (TASK_FS_TYPE §3)。実物の
# cmd_fs_shared.c + cmd_file.c を #include し、sys_stat と sys_ls の両方を IO にして、
# 呼び出し元 5 箇所が「不明」をファイルと読まず、open / mkdir / rename / unlink を
# 呼ばないことを受け手で見る。§6 は継承バグ台帳の `cp -r` — 失敗する経路で
# 宛先に空のディレクトリを残さない (収集してから mkdir / mkdir の戻り値を見る /
# 既に在るディレクトリへの上書きコピーだけは通す)。--mutate は否定側。
# 記録は tools/tests/fs_kind_callers_tdd.md。
check-fs-kind-callers-host:
	python3 -B tools/tests/test_fs_kind_callers.py --target --mutate

# `cat -n` の行番号は行の先頭でだけ出る。実物の cmd_fs_shared.c + cmd_file.c を
# #include し、sys_write(1, ...) に出た全バイトを試験側の素朴な参照実装と 1 バイト
# ずつ突き合わせる。(a) 改行で終わるファイルの後ろに空の行番号を出さない、
# (b) IO_BUF_SIZE (65536) の切れ目で行が終わったことにしない (行頭の状態を
# 読み取りをまたいで持つ) の 2 つ。§6 は継承バグ台帳の「内蔵 cat が標準入力を
# 読まない」— 引数が無ければ FD 0 を読み、FD 0 は閉じず、引数があれば読まない。
# --mutate は否定側で、(a) (b) と §6 の 3 つを壊した版が RED になることを見る。
# 記録は tools/tests/cat_linenum_tdd.md。
check-cat-linenum-host:
	python3 -B tools/tests/test_cat_linenum.py --target --mutate

# vfs_path_kind のプローブ (票 H1 / 往復 3 の B6)。実物の fs/vfs.c を #include し、
# 「読めなかったディレクトリ」が get_file_size 経由でファイルに化けないことを見る。
check-vfs-kind-host:
	python3 -B tools/tests/test_vfs_kind.py --target

# fstat がリダイレクトに従う / isatty と食い違わない (票 TASK_FSTAT_REDIR の F4 F5)。
# 実物の fs/vfs.c + fs/vfs_fd.c + **fs/fd_redirect.c** を #include し、リダイレクトは
# 贋物を置かずに実物を通す。`> file` なら fstat が実体 (S_IFREG・大きさ・時刻・inode)
# を答えること、パイプは S_IFIFO であること、そして **fstat が S_IFCHR ⇔ isatty が 1**
# が状態 (コンソール / ファイル / パイプ) × fd 0/1/2 の総当たりで成り立つことを見る。
# 判定の管理元が 1 か所 ([C4] fd_redirect_ifmt) であることは静的に突き合わせる。
# --mutate は否定側 (fstat だけ見ない版 / **isatty だけ見ない版** /
# パイプを S_IFCHR と答える版 / 実体ではなく作り話を返す版)。
# 記録は tools/tests/fstat_redir_tdd.md。
check-fstat-redir-host:
	python3 -B tools/tests/test_fstat_redir.py --target --mutate

# 読み取り失敗を「不存在」にしない (票 B8)。実物の ext2 を RAM ディスクへ載せ、
# **間接ブロックを使う大きなディレクトリ**の読み出しを一度だけ落として、
# 実物の vfs_open / vfs_open_sqlite まで通す。O_CREAT が既存ファイルを空に
# しないことを、FD・write_file の呼び出し回数・読み直した中身で押さえる。
# 2 巡目 (Codex 実装レビュー P1-4): 実物の fs/hostdrvfs.c を io.h の差し替えと
# 贋の NP21/W で動かし、OPEN の失敗が NOTFOUND に畳まれないことを見る。
check-b8-open-host:
	python3 -B tools/tests/test_b8_open.py --target
	python3 -B tools/tests/test_b8_hostdrv.py --target

# KAPI v50 (db_open_existing / prepare_only / bind_* / error_code、票 S0-K)。実 SQLite + 実 VFS + RAM backend。
check-db-v50-host:
	python3 -B tools/tests/test_kapi_db_v50.py

# db_last_error / db_column_text の返り先 (票 TASK_DB_ERRSTR)。実 SQLite + 実 kapi_db.c。
# 見るのは「返り先が共有メモリの範囲内か」と「結果を上限まで書いても診断文が壊れないか」。
# --mutate は否定側 (上限の引き算を 1 か所戻す / カーネル番地のまま返す / NUL を置き忘れる)。
check-db-errstr-host:
	python3 -B tools/tests/test_db_errstr.py --target

# libos32cfg / cfg コマンドのホスト TDD (票 S2-C)。実 SQLite + 実 kapi_db.c + RAM backend。
check-cfg-host:
	python3 -B tools/tests/test_cfg.py

# install --recover-settings / --revert-settings のホスト TDD (票 S3-I)。実 SQLite + 実 kapi_db.c + RAM backend。
check-install-recover-host:
	python3 -B tools/tests/test_install_recover.py

# install (無印) の通常インストール経路のホスト TDD (票 S3I2-I)。実 install.c + KAPI の贋物、--target で IdeInfo 96B の表明。
check-install-fresh-host:
	python3 -B tools/tests/test_install_fresh.py --target

# tools/host_agent.py v2 (ワイヤ v2 の Agent 側、票 N1 段 1)。贋 OS32 が
# フレームを直接組んで rid 台帳 / 3 way HELLO / 墓標 / 枯渇停止を踏む。
check-host-agent:
	python3 -B tools/tests/test_host_agent.py
# net/link.c (ワイヤ v2) + kapi/kapi_host.c (KAPI v51) のホスト TDD (票 N1 段 4)。
# 実物のソースを #include し、NIC / cli-sti / 100Hz タイマ / ディスパッチャだけを
# 贋物にする。対向は **実 Agent** (host_agent.py を UNIX ソケットで子プロセス起動)
# か台本。記録は tools/tests/n1_tdd.md。
check-net-link-host:
	python3 -B tools/tests/test_net_link.py --target
# libos32gui の os32gui_cfg_* wrapper の分岐 (票 S2-W)。C の実体は贋物。
check-gui-host:
	cargo test --manifest-path userland/rust/libos32gui/host_tests/Cargo.toml --target x86_64-unknown-linux-gnu --offline
# libos32host + wget/lpr/hclip/hdate のホスト TDD (票 N3)。実物のソースを #include し、
# KAPI / libos32host の関数だけを贋物に。記録は tools/tests/n3_tdd.md。
check-host-lib-host:
	python3 -B tools/tests/test_host_lib.py --target

# 試験プログラムの合否を機械が読める形にする約束事 (票 docs/tasks/test/
# TASK_TEST_RESULT.md §2 / §11)。終了コード (0 / 1 / 2、予約値 126/127/130/139 は
# 返さない) と最終行の集計行 `<名前>: PASS <n>/<m>` が**必ず一致する**ことと、
# その集計行が **fd 1 に出る**ことを固定する。実物の userland/lib/rt/testresult.h
# と、userland/tests/ の実物のプログラム 5 本 (stat_t / restest / test2 /
# klibc_test / font_load_test) を贋物の KernelAPI で**実際に走らせ**、
# **どの fd に何が書かれたか**と main の返り値の両方を観測する — grep では
# 一致は確かめられないし、画面 (kprintf) と fd 1 を同じバッファへ流す贋物では
# 票 §11 の穴 (16 本中 14 本の集計行がリダイレクトで拾えなかった) を見逃す。
# 静的側は第 1 陣 (票 §4) 16 本の `void main` / 名前が argv[0] 由来 /
# main の return が集計の答えでない / ヘッダの内部 (os32_test__*) を直接呼ぶ、
# を見る。Rust の alloc_demo は書式と終了コードを testresult.h と突き合わせる。
# --mutate は否定側 (どれもコンパイルは通る変異)。
# 記録は tools/tests/result_conv_tdd.md。
check-result-conv-host:
	python3 -B tools/tests/test_result_conv.py --target --mutate

# ゲストで一括実行してホストで集計するランナー (票 docs/tasks/test/
# TASK_TEST_RUNNER.md)。**2 つのターゲットは別物なので混ぜないこと。**
#
#   check-guest-host  ランナーの**ホスト試験** (受入 R7)。NP21/W に触らない。
#                     生成 (一覧 → 平らなスクリプト) と集計 (出力 → 判定) は
#                     エミュレータにも時計にも触らない純関数に切ってあるので、
#                     贋物の入力だけで全部踏める。**そこが壊れていたらゲストで
#                     回しても意味がない**ので、これは `check` の列に入れる
#                     (実物の tools/guest_tests.py を書き換えて戻す変異試験を
#                     持つので check-mut 側 = 逐次)。
#                     --mutate の否定側: 食い違い (0 なのに FAIL) の見逃し /
#                     見張りが発火しない / 固まった試験を名指ししない /
#                     /host が無いのに合格にする / 落ちた試験 (139) で後続を
#                     打ち切る / 前回の出力が混ざる (R8)。
#                     記録は tools/tests/guest_tests_tdd.md。
#
#   check-guest       **本番。ゲストで実際に走らせる。**`make check` の列には
#                     入れない — NP21/W が動いている必要があり ([D1] の領域)、
#                     `make check` はホストだけで完結する約束だから (票 §4)。
#                     走らせる一覧は tools/tests/guest_tests.txt。
check-guest-host:
	python3 -B tools/tests/test_guest_tests.py --mutate

check-guest:
	python3 tools/guest_tests.py

# kstring の C 版 (移植性準備の順序 4-b)。実物の lib/kstring_asm.asm (nasm) と
# 実物の lib/kstring_c.c を **同じ実行ファイルにリンク**し、13 本すべてを同じ
# 入力で突き合わせる (戻り値とバッファの全内容が一致すること)。x86 の既定
# ビルドはアセンブリのままなので、これは切り替えの門ではなく答え合わせ。
# --mutate は否定側 (kstrcmp を符号付きにすると日本語ファイル名の並び順が
# アセンブリ版と食い違って落ちる、など)。記録は tools/tests/kstring_c_host.c。
check-kstring-c-host:
	python3 -B tools/tests/test_kstring_c.py --mutate

# kstring の実測プログラム kstr_bench の**計測の枠組み** (票
# docs/tasks/portability/TASK_KSTRING_BENCH.md)。実物の
# userland/tests/kstr_bench.c を 1 行も写さず #include し、KernelAPI
# (get_tick / sys_write / sys_yield) と測られる 13 本だけを贋物にして回す。
# 見るのは数字ではなく**数字の作り方**: 出力の固定書式、1 ケース 1MB 以上 →
# 30 ティック未満なら倍 (倍は 5 回まで = 時計が止まっても終わる)、食い違いを
# 注入したら MISMATCH が出てその関数の計測が飛ぶこと、13 本が 4 者 (.asm の
# global / 表 / 改名表 / 贋物) で一致すること、[V2] の登録。
# 最後に**実物の .asm と .c を同居させた版**も回して 13 本の一致を見る (受入 K1)。
# --mutate は否定側 (倍にしない版 / 欄を入れ替えた版 / 飛ばさない版 など)。
# 記録は tools/tests/kstr_bench_tdd.md。
check-kstr-bench-host:
	python3 -B tools/tests/test_kstr_bench.py --target --mutate

# ARM コンパイル計測 (移植性準備の順序 1)。カーネル側の C ソースを 1 本ずつ
# arm-none-eabi-gcc に通し、通った本数と失敗の分類を出す。
#
# **`check` の列にはわざと入れていない。** これは合否の門ではなく計測器で、
# 今 ARM で通らないのは当たり前 (x86 前提でよい、と決めて書いてある)。
# 門にすると「直さないと緑にならない」圧力がかかり、まだ設計の決まっていない
# arch/ の分離を急がせてしまう。io.h 経由への統一 (順序 2) や arch/ 導入
# (順序 3) の効果を同じ物差しで見るために、独立したターゲットとして呼ぶ。
#
# arm-none-eabi-gcc が無い環境では SKIP して終了コード 0。
check-arm-compile:
	@python3 tools/check_arm_compile.py

# 文書のリンク切れ検査 (lychee の薄い包み)。相対パスの実在と見出しアンカーの
# 実在を見る。900 リンクで 0.03 秒なので `check` の列に入れてある。
# lychee (cargo install lychee) が無い環境では SKIP して終了コード 0。
check-docs-links:
	@python3 tools/check_docs_links.py

# 孤児文書の検出 — docs/INDEX.md から辿れない docs/*.md と、どの票からも
# 参照されていない tools/tests/*_tdd.md。lychee の守備範囲外なので自前。
#
# **`check` の列にはまだ入れていない。** 2026-09-15 の棚卸し時点で 31 本 +
# 20 本が未参照で、これは検査の不備ではなく索引の取りこぼし (票を書いて
# INDEX.md に載せ忘れたもの) の実数。今これを門にすると、通すために
# `docs/.orphans-allow` へ全部書き写すことになり、例外表が「黙らせる表」に
# 化けて二度と減らない。**PM が文書整理の段階 F で索引を直し (載せるか
# archive へ移すか)、0 になった時点で `check` の列へ移すこと。**
check-docs-orphans:
	@python3 tools/check_docs_orphans.py

# 試験一覧 docs/TESTS.md の鮮度検査 (文書整理 段階 E)。表は build/*.mk と試験
# スクリプトから tools/gen_tests_inventory.py が生成するので、ターゲットを足した
# のに一覧が古いままという状態を止める。check_kapi_version.py と同じ「生成物と
# 正典の照合」の作法で、ずれたら --write を促して落ちる。手書きの節は
# docs/TESTS.md の `<!-- manual:… -->` 区間だけで、生成器はそこを読み戻して保つ。
check-tests-inventory:
	@python3 tools/gen_tests_inventory.py --check

# check は**必ず逐次**で回す (2026-09-17)。変異試験は実物のソースを書き換えて
# 戻す作りなので、同時に走ると互いのファイルを壊し合う。しかも壊れ方が
# 再現しない (docs/POLICY_DEBUG.md §4-40)。Makefile が既定で -j を足すので、
# ここで -j1 を明示して打ち消す。並列化するには各試験が写しの上で変異する
# 作りに変える必要があり、それは別作業。
# check は 2 段。**遅さの正体は逐次ではなく、変異試験が同じソースを奪い合う
# ことだった** (2026-09-17)。変異試験は実物のソースを書き換えて戻す作りなので、
# 同時に走ると互いのファイルを壊し合い、しかも壊れ方が再現しない
# (docs/POLICY_DEBUG.md §4-40)。
#
#   1 段目 check-par  書き換えない 48 本 → **並列**
#   2 段目 check-mut  --mutate を渡す 10 本 → **逐次 (-j1)**
#
# 各段の後で tools/check_tree_unchanged.py が「試験がソースを書き換えたまま
# 戻していないか」を見る。1 段目で引っかかれば、その試験を 2 段目へ移すこと。
# 全部を並列にするには各試験が写しの上で変異する作りに変える必要がある。
check:
	@python3 tools/check_tree_unchanged.py --save par
	@$(MAKE) check-par
	@python3 tools/check_tree_unchanged.py --verify par
	@python3 tools/check_tree_unchanged.py --save mut
	@$(MAKE) -j1 check-mut
	@python3 tools/check_tree_unchanged.py --verify mut

check-par: check-kapi-version check-docs-links check-docs-orphans check-tests-inventory check-manifests check-constraints check-privileged check-arch-asm check-le-access check-ne2000-ring check-shlib check-gui-proto check-term-model check-term-render check-t5a-host check-memory-host check-memmap-host check-memmap check-boot-splash-host check-tools-host check-gshell-host check-db-owned-host check-vfs-fd-sqlite-host check-vfs-mount-dev-host check-sqlite-groups-host check-con-sink-host check-kbd-inject-host check-launch-host check-ring3-str-host check-sh-launch-host check-sh-shell-host check-sh-truncation-host check-multiapp-model-host check-settings-protect-host check-hsync-h1-host check-hostdrv-list-host check-fs-kind-host check-vfs-kind-host check-b8-open-host check-db-v50-host check-db-errstr-host check-cfg-host check-gui-host check-install-recover-host check-install-fresh-host check-host-agent check-net-link-host check-host-lib-host

check-mut: check-fstat-redir-host check-kstring-c-host check-kstr-bench-host check-sh-status-host check-hsync-h3-host check-hsync-h2-host check-h4-manifest-host check-vfs-excl-host check-fs-kind-callers-host check-cat-linenum-host check-result-conv-host check-guest-host

clean-sdk:
	rm -rf $(SDK_OUT) $(SDK_DIST_DIR)

.PHONY: check-memmap check-memmap-host sdk sdk-dist clean-sdk check-fstat-redir-host check-vfs-excl-host check-hsync-h2-host check-h4-manifest-host check-kapi-version check-manifests check-constraints check-privileged check-arch-asm check-le-access check-gui-proto check-term-model check-term-render check-t5a-host check-memory-host check-memmap-host check-memmap check-boot-splash-host check-tools-host check-gshell-host check-db-owned-host check-vfs-fd-sqlite-host check-vfs-mount-dev-host check-sqlite-groups-host check-con-sink-host check-kbd-inject-host check-launch-host check-ring3-str-host check-sh-launch-host check-sh-shell-host check-sh-truncation-host check-sh-status-host check-multiapp-model-host check-settings-protect-host check-hsync-h1-host check-hsync-h3-host check-hostdrv-list-host check-fs-kind-host check-fs-kind-callers-host check-cat-linenum-host check-vfs-kind-host check-b8-open-host check-db-v50-host check-db-errstr-host check-cfg-host check-gui-host check-install-recover-host check-install-fresh-host check-host-agent check-net-link-host check-host-lib-host check-kstring-c-host check-kstr-bench-host check-result-conv-host check-guest-host check-guest check-arm-compile check-docs-links check-tests-inventory check-docs-orphans check
