# T5b display-only — TDD / review handoff

## Scope and invocation

- Production: `src/terminal.rs`, resident loop connection in `lib.rs`, fixed overlay integration in `wm.rs/visible.rs/input.rs/startmenu.rs`, one existing-symbol declaration in `ffi.rs`, local Cargo dependencies/lock.
- No changes to SessionAction, exec implementation, FD capture, T4/T5R/T5a, SDK/KAPI or shared build rules. Existing dirty files outside gshell are not this lane's work.
- Core: `python3 userland/gshell/host/run.py` (10 tests).
- Full host integration: `python3 userland/gshell/host/integration.py` (28 tests, includes the same 10 core tests). Do not sum these as 38 distinct tests.
- `integration.py` compiles the actual gshell modules and the actual resident-loop functions against explicit host KAPI/GFX substitutes. Only the test crate's entry point/no_std/module paths are adapted. SDK sources are read, never changed. Artifacts go in `target/t5b-host`.
- Host SHM tests require Linux x86_64 `MAP_32BIT`, because actual gshell slot addressing uses u32. Mock calls are not proof of guest heap restoration, backend behavior, or font ROM correctness.

## Observed RED → GREEN sequence

This is a record of assertions observed during implementation, not a fabricated raw transcript. Each listed behavior was first run against absent/no-op behavior, then implemented and run GREEN. Tests were initially beside production code, and moved unchanged in meaning to `host/terminal_tests.rs` after GREEN.

| Test / behavior | Observed first assertion RED |
|---|---|
| `heap_scope_initializes_valid_cells_and_repeated_close_frees_once` | `None` vs `Some(42)`; allocation failure branch never lends null, 20 repeated scopes free once each |
| `normal_fixture_streams_are_independent_and_saved` | `Single(' ')` vs `Wide('日')` |
| `boundary_fixtures_report_stop_pending_and_unconsumed_separately` | four normal `(Complete,229,229,...)` reports vs Exact/Full/TooWide/Finish reports |
| `top_level_retains_across_child_callbacks_and_cui_failure_then_drops` | driver step count `1` vs `4` |
| `controls_reset_scopes_close_reopen_and_refusal_is_not_retried` | top `Some(0)` vs `Some(1)` |
| `saved_panel_clip_holes_and_half_japanese_reexposure` | `layout(...).is_some()` false |
| `pointer_and_both_edges_do_not_leak_after_leaving_or_closing_panel` | hover sample not consumed |
| `invalid_view_and_region_overflow_fail_before_any_write_or_glyph` | pixel output nonempty for invalid top; preflight now precedes all writes |
| `headers_show_controls_and_stop_outside_body` | empty header vs `stdout* stderr   fixture   Close` |
| `guest_top_level_open_close_uses_parent_heap_and_invalidates` | `request_open()` false |
| `compositor_occludes_app_and_repairs_actual_full_chrome_writes` | panel pixel color `1` vs `7` |
| `actual_x3_x4_mouse_capture_has_no_panel_to_app_leaks_or_glyph_work` | app ring pending `1` vs `0` |
| `app_drag_outline_stays_below_fixed_panel` | pixel at `(30,110)` color `13` vs `7` |
| `mouse_controls_defer_model_mutation_and_preserve_first_display_request` | pending `None` vs `Some(Tab)` |
| `fixture_and_top_mouse_rows_match_labels` | pending `None` vs `Some(Select(Normal))`; fixture click test coordinates were corrected to actual label columns before implementation, then RED rerun |
| `z_start_menu_display_never_overwrites_pending_session_action` | pending `None` vs `Some(Open)` |
| `resident_standalone_loop_actually_services_display_and_drops_on_exit` | halt count `0` vs `2` |
| `small_screen_refuses_before_allocating` | refusal modal not open |
| `direct_x4_redraw_is_rejected_before_glyph_lookup` | glyph count increased from `3695` to `4434` |
| `recursive_driver_cannot_allocate_or_consume_outer_pending` | `reentered top-level driver` assertion panic |
| Additional WM-drag pointer case in actual X3/X4 test | app ring pending `1` vs `0`; fixed final pointer forwarding gate |

The four `regression_*` tests are separate regressions, not claimed as new assertion-RED history:
- app visible-region overflow remains a subset excluding the panel;
- existing `run_program` callback retains parent model/storage until return, including a slotless mocked child and opaque negative return;
- existing CUI write failure preserves model; success ends the scope before free;
- actual modal/menu/FEP/cursor pixels survive read-only panel redraw.

The CUI regression initially aborted because the host fixture lacked `ime_is_active`; this was a **mock setup failure**, not assertion RED. Adding that explicit mock allowed the existing path to execute. No guest pass is inferred.

## Focused capture-release review fix

- Exact `input::capture(Ctx::Wait)` reproducer: panel `(100,150,1)` → taskbar `(500,screen_h-1,0)` → app `(500,200,1)`, with no idle sample. Added the test before production edits. First integration run: **24 passed / 1 failed**, app ring `left: 0`, `right: 1`. After bypass bookkeeping: **25 passed / 0 failed**.
- Added modal sibling regression before its fix. Second RED: **25 passed / 1 failed**, again app ring `left: 0`, `right: 1`. Modal returns before `terminal::mouse`, justifying the single added `terminal::release_buttons(btn)` call in `input.rs`. After that change: **26 passed / 0 failed**.
- `release_buttons` only intersects captured bits with physical buttons. Menu/taskbar/WM-drag bypasses still return false and keep upper UI delivery priority. Modal delivery and Pump's deferred `prev_buttons` are unchanged. Normal panel handling still consumes its release sample before clearing held bits.
- FEP is not an early return: it excludes the hit, but still runs `PointerGate::sample`, so its release already clears capture. Added menu and FEP integration regressions (not claimed as separate RED cycles); both check no release leak and immediate next app press. Menu remains open until explicitly closed; the FEP test uses a real FEP rectangle inside the panel.
- Final `python3 userland/gshell/host/integration.py`: **28 passed / 0 failed**, exit 0. `python3 userland/gshell/host/run.py`: **10 passed / 0 failed**, exit 0 (subset of integration).
- `make gshell`: exit 0; OS32X `text=174048, bss=17936, heap=1048576, entry=0x0, load=0x300000, api>=42, total=174092`. Linker warned about missing `.note.GNU-stack` in `libc_a-sbrkr.o` and an RWX LOAD segment. No guest execution is claimed.
- `git diff --check -- userland/gshell/src/input.rs` and `rustfmt --edition 2021 --check userland/gshell/src/terminal.rs userland/gshell/host/terminal_tests.rs`: exit 0. Input's pre-existing rustfmt differences were left untouched.
- Focused edits only: `src/terminal.rs`, one line in `src/input.rs`, `host/terminal_tests.rs`, this log. The older `verification.txt` describes the pre-review run; the exact post-review results are above. No shared build/docs edits, emulator, deploy, network, secrets, commit, or agents.

## Ownership / rendering review points

- `drive` is a top-level scope guard, held across the existing synchronous step/exec path even without GUI owner slots. Recursive entry does not consume pending or allocate.
- `with_cells` initializes each heap Cell with `BLANK` in place before making a slice. Its closure cannot return a storage borrow. Stack Panel/Terminals end before free.
- Fixture reset exits the prior scoped Panel before reborrowing the same external heap storage. No self-referential struct or static Terminal lifetime exists.
- A separate borrow guard surrounds saved-model readers, including font/sink callbacks. Nested reads fail closed. Model edits happen only between driver steps. X4 can update small request/capture flags; `draw` also explicitly rejects `in_pump`.
- Z is fixed: apps/chrome/drag outline < panel < modal/taskbar/menu/FEP/cursor. Input includes both buttons and release outside/after close. Existing WM drags retain their drop path but cannot forward pointer through the panel.
- Chrome does not obey the requested clip. `wm::recompose_panel` therefore repairs the **entire panel plus complete upper overlays** after actual chrome/drag writes and queues those actual repaired rectangles. This may be expensive on slow hardware; no performance acceptance is claimed.
- App Paint visibility subtracts the panel fail-closed; close invalidates underlying clients and recomputes exposure. Rendering preflights geometry/region capacity before writes; no full-region fallback on overflow.

## Build / remaining PM registration

`make gshell` links successfully using existing `build/programs.mk`'s FORCE Cargo rule. The path dependencies are `../libos32term` and `../libos32term_render`; Cargo tracks them without a new shared make dependency. `unicode_to_jis` is already supplied by the existing program UTF-8 object in libos32gfx.

Shared build files were not edited. To include this lane in normal checks, PM should add a target in `build/sdk.mk`:

```make
check-t5b-host:
	python3 userland/gshell/host/integration.py
```

Add `check-t5b-host` to that file's existing `check:` prerequisites and `.PHONY` list. No new deploy entry, child BIN, KAPI or link library is needed.

`make check` passed separately, including unchanged T4/T5R/T5a host regressions. Its manifest checker lists existing intentionally unregistered/incremental artifacts; this is not a guest test. Final run/build output is recorded in `verification.txt`.

No emulator, deploy, network, environment/secrets, hardware mirror, commit, or additional agent was used. Independent review and guest validation remain outstanding; acceptance order remains T5a → T5b → T6a.
