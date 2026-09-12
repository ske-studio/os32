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
SDK_LIB_HEADER_DIRS = math gfx db ui input asset snd tilemap md filer mgx save ecs

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

check-boot-splash-host:
	python3 -B tools/tests/test_boot_splash_native.py

check-tools-host:
	python3 -B -m unittest discover -s tools/tests -p 'test_np21w_*.py'
	python3 -B tools/tests/test_nhd_deploy_failure.py
	python3 -B tools/tests/test_filer_normalize.py
	python3 -B tools/tests/test_filer_copy_abort.py
	python3 -B tools/tests/test_gui_button_dispatch.py
	PYTHONPATH=. python3 -B tools/tests/test_emu_playbook.py

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

check: check-kapi-version check-manifests check-constraints check-privileged check-ne2000-ring check-shlib check-gui-proto check-term-model check-term-render check-t5a-host check-memory-host check-boot-splash-host check-tools-host check-gshell-host check-db-owned-host check-vfs-fd-sqlite-host check-vfs-mount-dev-host check-sqlite-groups-host check-con-sink-host check-kbd-inject-host check-launch-host check-sh-launch-host check-sh-shell-host check-multiapp-model-host

clean-sdk:
	rm -rf $(SDK_OUT) $(SDK_DIST_DIR)

.PHONY: sdk sdk-dist clean-sdk check-kapi-version check-manifests check-constraints check-privileged check-gui-proto check-term-model check-term-render check-t5a-host check-memory-host check-boot-splash-host check-tools-host check-gshell-host check-db-owned-host check-vfs-fd-sqlite-host check-vfs-mount-dev-host check-sqlite-groups-host check-con-sink-host check-kbd-inject-host check-launch-host check-sh-launch-host check-sh-shell-host check-multiapp-model-host check
