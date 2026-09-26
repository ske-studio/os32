#!/usr/bin/env python3
"""Compile unchanged gshell modules against an explicitly mocked host ABI.
No SDK edits; generated adapters live in target/gshell-host only.

--mutate: after the normal run, apply each mutation in MUTATIONS to a
temporary copy of userland/gshell/{src,host} (the source tree is never
written) and require the named tests to go RED.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'userland/gshell/target/gshell-host'
OUT.mkdir(parents=True, exist_ok=True)
def run(args):
    subprocess.run(args, cwd=ROOT, check=True)
sys.path.insert(0, str(ROOT / 'tools/tests'))
import os32api_host
os32api_host.build(OUT)


def build(gshell: Path, name: str, quiet: bool = False) -> Path:
    """gshell の src/lib.rs を試験用の根に書き換えて rustc --test でビルドする。"""
    src = gshell / 'src'
    root = (src / 'lib.rs').read_text().replace('#![no_std]', '#![allow(dead_code)]').replace('#[no_mangle]', '').replace('pub extern "C" fn main(', 'pub extern "C" fn guest_main(')
    root = re.sub(r'^mod (\w+);', lambda m: '#[path="' + str(src/(m[1]+'.rs')) + '"] mod '+m[1]+';', root, flags=re.M)
    root += '\n#[path="'+str(gshell/'host/mocks.rs')+'"] mod mocks;\n'
    rs = OUT / (name + '_root.rs')
    rs.write_text(root)
    exe = OUT / (name + '-tests')
    args = ['rustc', '--edition=2021', '--test', str(rs), '-L', str(OUT), '--extern', 'os32api='+str(OUT/'libos32api.rlib'), '-o', str(exe)]
    if quiet:
        subprocess.run(args, cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        run(args)
    return exe


# 票 KBD_NAV (docs/tasks/gui/TASK_KBD_NAV.md) の受け入れ K1 の変異。
# (名前, ファイル, 置き換え前 (ちょうど 1 か所), 置き換え後, RED になるべき試験の絞り込み)
MUTATIONS = [
    ('switch ignores MRU (Z order only)', 'src/kbdnav.rs',
     '        push(out, &mut n, st.kn.mru[i]);', '        let _ = st.kn.mru[i];',
     'k1_1_minimized_window_is_last_in_mru'),
    ('switch on every TAB (not on GRPH release)', 'src/kbdnav.rs',
     '        advance_switch(st, back);\n    } else if back {',
     '        advance_switch(st, back);\n        finish_switch(st);\n        return;\n    } else if back {',
     'k1_1_grph_tab_switches_once_on_release'),
    ('ESC does not cancel the switch', 'src/kbdnav.rs',
     '            cancel_switch(st);\n            return true;', '            return true;',
     'k1_1_grph_tab_esc_cancels'),
    ('taskbar presses the front, not the selection', 'src/taskbar.rs',
     '        Some(id) => id,\n        None => st.front_id(),',
     '        Some(_) => st.front_id(),\n        None => st.front_id(),',
     'k1_1_grph_tab_switches_once_on_release'),
    ('GRPH+f4 during modal does not cancel', 'src/kbdnav.rs',
     '            modal::on_key(st, SC_ESC, 0x1B, 0);', '',
     'k1_2_modal_swallows'),
    ('modal does not stop the shortcuts', 'src/kbdnav.rs',
     '    if modal::is_open() {\n        if k == Shortcut::Close {',
     '    if false {\n        if k == Shortcut::Close {',
     'k1_2_modal_swallows'),
    ('X4 runs WM keys instead of deferring', 'src/input.rs',
     'if defer || (ctx == Ctx::Pump && kbdnav::is_wm_raw(st, raw)) {', 'if defer {',
     'k1_3_pump_defers'),
    ('X4 does not defer the kana raw', 'src/kbdnav.rs',
     '    if scan == SC_KANA {\n        return true;\n    }\n', '',
     'k1_4_kana_release_read_by_the_pump'),
    ('kana mode from the current value, not the raw bit', 'src/kbdnav.rs',
     '        set_mousekeys(st, (mods & MOD_KANA) != 0);',
     '        set_mousekeys(st, (unsafe { (os32api::api().kbd_get_modifiers)() } & MOD_KANA) != 0);',
     'k1_4_kana_bit_is_taken_from_each_raw'),
    ('kana off keeps the held button', 'src/kbdnav.rs',
     '    if !on {\n        release_held(st);\n    }', '',
     'k1_4_held_button_is_released'),
    ('consumed numpad break leaks to the app', 'src/kbdnav.rs',
     '    if c {\n        set_consumed(st, scan, true);\n    }', '',
     'k1_4_numpad5_is_a_click'),
    ('mousekey edge bypasses the modal', 'src/input.rs',
     'pub fn edge_x3(st: &mut GuiState, x: i32, y: i32, button: u8, down: bool) {\n    if modal::is_open() {',
     'pub fn edge_x3(st: &mut GuiState, x: i32, y: i32, button: u8, down: bool) {\n    if false {',
     'k1_4_numpad5_during_modal'),
    ('minus is a left click', 'src/kbdnav.rs',
     '        NP_MINUS => click(st, BTN_RIGHT),', '        NP_MINUS => click(st, BTN_LEFT),',
     'k1_4_minus_is_a_right_click'),
    ('plus is a single click', 'src/kbdnav.rs',
     '            click(st, BTN_LEFT);\n            click(st, BTN_LEFT);', '            click(st, BTN_LEFT);',
     'k1_4_plus_is_a_double_click'),
    ('acceleration 4 -> 2', 'src/kbdnav.rs',
     'const MK_ACCEL: [(u32, i32); 3] = [(7, 1), (15, 4), (u32::MAX, 8)];',
     'const MK_ACCEL: [(u32, i32); 3] = [(7, 1), (15, 2), (u32::MAX, 8)];',
     'k1_5_numpad6_twenty_makes'),
    ('break does not reset the count', 'src/kbdnav.rs',
     '                st.kn.mk_count = 0; /* break で数え直し */', '',
     'k1_5_numpad6_twenty_makes'),
    ('the still real mouse rewinds the pointer', 'src/input.rs',
     'let (mx, my) = if real_moved { (rx, ry) } else { (st.mouse_x, st.mouse_y) };',
     'let (mx, my) = (rx, ry);',
     'k1_5_numpad6_twenty_makes'),
    ('CTRL is ignored', 'src/kbdnav.rs',
     '        let slow = (mods & MOD_CTRL) != 0;', '        let slow = false;',
     'k1_8_ctrl_numpad6'),
    ('the half-dot remainder is dropped', 'src/kbdnav.rs',
     '    *acc -= d * MK_SLOW_DEN;', '    *acc = 0;',
     'k1_8_ctrl_numpad6'),
    ('mousekey move does not follow the drag', 'src/input.rs',
     '    if st.drag_index >= 0 {\n        update_drag(st, x, y);\n    } else {\n        cursor::move_to(st, x, y);\n    }\n    let b',
     '    cursor::move_to(st, x, y);\n    let b',
     'k1_7_drag'),
    ('no FEP commit before a WM focus change', 'src/wm.rs',
     '    if x3 {\n        fep::commit_to(st, old);\n    } else {', '    if x3 {\n    } else {',
     'k1_6_'),
    ('the commit goes to the new focus', 'src/fep.rs',
     '    let t = input::target_of(st, win_id);', '    let t = input::focus_target(st);',
     'k1_6_app_set_focus'),
    ('X1 set_focus runs the conversion at once', 'src/wm.rs',
     '        fep::defer_commit(old);', '        fep::commit_to(st, old);',
     'k1_6_app_set_focus'),
]


def one_mutation(k: int) -> tuple:
    """変異 k を一時の写しに当ててビルドし、絞り込んだ試験を走らせる。(ok, 行)。"""
    name, rel, old, new, filt = MUTATIONS[k]
    with tempfile.TemporaryDirectory() as td:
        g = Path(td) / 'gshell'
        shutil.copytree(ROOT / 'userland/gshell/src', g / 'src')
        shutil.copytree(ROOT / 'userland/gshell/host', g / 'host')
        f = g / rel
        text = f.read_text()
        if text.count(old) != 1:
            return False, f'MUTATION BROKEN  {name}: {rel} has {text.count(old)} matches'
        f.write_text(text.replace(old, new))
        try:
            exe = build(g, f'mutant{k}', quiet=True)
        except subprocess.CalledProcessError:
            return False, f'MUTATION NOT BUILT  {name}'
        r = subprocess.run([str(exe), '--test-threads=1', filt], cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        exe.unlink(missing_ok=True)
        ran = re.search(r'test result: \w+\. (\d+) passed; (\d+) failed', r.stdout)
        if r.returncode != 0 and ran and int(ran[2]) > 0:
            return True, f'RED  {name}  ({ran[2]} failed)'
        return False, f'SURVIVED  {name}  (filter {filt!r})'


def mutate() -> int:
    import concurrent.futures
    import os
    jobs = max(1, min(8, os.cpu_count() or 1))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        results = list(ex.map(one_mutation, range(len(MUTATIONS))))
    bad = 0
    for ok, line in results:
        print(line)
        if not ok:
            bad += 1
    print(f'kbdnav mutations: {len(MUTATIONS) - bad}/{len(MUTATIONS)} killed')
    return bad


exe = build(ROOT / 'userland/gshell', 'integration')
run([str(exe), '--test-threads=1'])
if '--mutate' in sys.argv[1:]:
    sys.exit(1 if mutate() else 0)
