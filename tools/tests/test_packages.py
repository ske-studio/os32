#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CD のパッケージが配備マニフェストと一致しているか (make check-packages-host)

CD インストール (userland/system/cdinst.c) の .PKG は、配備の正典
(build/core.yaml + userland/deploy.yaml) のタグから tools/mkpkg.py --plan が
作る (構成は build/packages.yaml)。2026-09-24 までは一覧を手で写していて、
配備 179 本のうち 27 本 (gshell / libos32gui.shlib / 既定フォント …) が CD に
入っていなかった。ここで見るのは:

  1. 構成の検査 (実物): 配備マニフェストの各行がちょうど 1 つのパッケージに
     当たるか、除外に理由があるか、userland/tests/ 由来が test タグか
  2. 否定側: 当たらないタグ / 2 つに当たる / tests 由来が programs /
     理由の無い除外 / 古い除外 / ゲストパスの重複 をそれぞれ報告するか
  3. 分割: 128 項目を超えると NAME, NAME2, … に分かれ、どれも 128 以下
  4. 媒体 (実物): 実物の mkpkg で作った PKG を読み戻し、配備の全ファイル
     (除外以外) + 媒体だけの物が**同じバイト列で**ちょうど 1 回ずつ入っているか
  5. ISO (実物): images/os32_install.iso の中の PKG を取り出して 4 と同じ検査
     (isoinfo が無ければ SKIP と表示する)
  6. cdinst.c のベース名が構成の名前と一致し、展開の順が MINIMAL → GUI →
     NORMAL → DEBUG で、pkg.h の上限が mkpkg と一致するか
  7. 起動 FD: build/image.mk が一覧を手で持たず、FD の中身が BOOT + MINIMAL
     (+ fd.only) と**等しい**か。構成 (fd_plan) と、実物のイメージ
     (images/os32_boot.img = 2HD の D88 のもと、images/os32_boot144.img) を
     FAT12 として読み戻してバイト列で見る。否定側 (8.3 違反・理由なし・古い
     rename) も。空きを表示する

先に make all を通してから実行すること (4 と 5 は成果物を読む)。
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import deploy_manifests as dm  # noqa: E402
import mkpkg  # noqa: E402

PLAN = os.path.join(ROOT, 'build', 'packages.yaml')
ISO = os.path.join(ROOT, 'images', 'os32_install.iso')
CDINST = os.path.join(ROOT, 'userland', 'system', 'cdinst.c')
PKG_H = os.path.join(ROOT, 'userland', 'lib', 'rt', 'pkg.h')

fails = []

FD_IMAGES = [('2HD', os.path.join(ROOT, 'images', 'os32_boot.img'),
              'boot/loader_fat_new.bin'),
             ('1.44MB', os.path.join(ROOT, 'images', 'os32_boot144.img'),
              'boot/loader_fat144.bin')]
IMAGE_MK = os.path.join(ROOT, 'build', 'image.mk')

# MINIMAL の外部コマンド (タグ base)。cdinst の「CUI only: shell + basic commands」。
# 2026-09-24 までの FD の FDD_MIN_CMDS と同じ顔ぶれ (diff / du / cal / man / cfg は
# そのとき FD にだけあった)
MINIMAL_BASE_CMDS = ['more', 'less', 'grep', 'find', 'sort', 'head', 'tail', 'wc',
                     'tee', 'touch', 'hexdump', 'sleep', 'diff', 'du', 'cal', 'man',
                     'sndctl', 'cfg']
# GUI.PKG (タグ gui) の中身。shlib を使う試験アプリは test (DEBUG) のまま
GUI_FILES = ['/bin/gshell.bin', '/sys/lib/libos32gui.shlib',
             '/usr/bin/filer.bin', '/usr/bin/edit_gui.bin']


def check(cond, label):
    print(f"  {'ok  ' if cond else 'FAIL'} {label}")
    if not cond:
        fails.append(label)
    return cond


# ---------------------------------------------------------------- 1. 実物の構成

def case_real_plan():
    print("case 1: 実物の構成 (build/packages.yaml + 配備マニフェスト)")
    plan = mkpkg.load_plan(PLAN)
    resolved, problems = mkpkg.expand_plan(plan, ROOT)
    for p in problems:
        print(f"       {p}")
    check(problems == [], "振り分けに問題が無い")
    for name, _, _, files in resolved:
        nent = len(files) + len(mkpkg.entry_dirs(g for g, _ in files))
        check(nent <= mkpkg.PKG_MAX_ENTRIES,
              f"{name.upper()}.PKG: {len(files)} files, {nent} entries <= "
              f"{mkpkg.PKG_MAX_ENTRIES}")
    # Minimal =「CUI のシェル + 基本コマンド」(cdinst の選択肢 1 の文言)
    minimal = {g for n, _, _, fs in resolved if n == 'minimal' for g, _ in fs}
    need = ['/sys/shell.bin', '/sbin/install.bin', '/sbin/cdinst.bin',
            '/sys/font/default.kcgfont', '/boot/vmkernel.lz4', '/sys/unicode.bin'] + \
        ['/bin/%s.bin' % c for c in MINIMAL_BASE_CMDS]
    lack = [g for g in need if g not in minimal]
    check(not lack, f"MINIMAL にシェルと基本コマンドと既定フォントが入っている (欠け {lack})")
    # GUI は MINIMAL に無く、GUI.PKG が gui タグの行とちょうど一致する
    gui = sorted(g for n, _, _, fs in resolved if n.startswith('gui') for g, _ in fs)
    check(not (set(GUI_FILES) & minimal), "MINIMAL に GUI (gshell / shlib / GUI アプリ) が無い")
    man = dm.load_merged(dm.CORE_MANIFEST_RELPATHS)
    tagged = sorted(g for e in man['filesystem']['files'] if 'gui' in (e.get('tags') or [])
                    for _, g in dm.resolve_entry(e, ROOT))
    check(gui == tagged, f"GUI.PKG の中身 = gui タグの行 ({len(gui)} 件)")
    check(gui == sorted(GUI_FILES), f"GUI.PKG の顔ぶれ {gui}")
    order = [n for n, _, _, _ in resolved]
    check([n for n in order if not n[-1].isdigit()] == ['boot', 'minimal', 'gui', 'normal', 'debug'],
          f"パッケージの並びが依存の順 {order}")
    return plan, resolved


# ---------------------------------------------------------------- 2. 否定側

def fake_manifest(files, loader='boot/loader_hdd.bin'):
    for e in files:
        e.setdefault('_manifest', 'fake.yaml')
    return {'boot': {'loader': loader},
            'filesystem': {'directories': [], 'files': files}}


def fake_plan(exclude=None, extra_pkg=None):
    pk = {
        'boot': {'type': 'boot', 'files': [
            {'host': 'boot/loader_hdd.bin', 'guest': '/boot/loader_hdd.bin'}]},
        'minimal': {'tags': ['core']},
        'normal': {'tags': ['programs', 'docs', 'data']},
        'debug': {'tags': ['test']},
    }
    if extra_pkg:
        pk.update(extra_pkg)
    return {'packages': pk, 'exclude': exclude or []}


def problems_of(plan, files, loader='boot/loader_hdd.bin'):
    _, probs = mkpkg.plan_packages(plan, ROOT, fake_manifest(files, loader))
    return probs


def case_negative():
    print("case 2: 否定側 (作った配備マニフェストで)")
    base = [{'host': 'userland/shell.bin', 'guest': '/sys/shell.bin', 'tags': ['core']}]

    p = problems_of(fake_plan(), base)
    check(p == [], "正常系は問題なし")

    p = problems_of(fake_plan(), base + [
        {'host': 'userland/cmds/ls.bin', 'guest': '/bin/ls.bin', 'tags': ['tools']}])
    check(any('当たらないタグ' in x for x in p), "当たらないタグを報告する")

    p = problems_of(fake_plan(), base + [
        {'host': 'userland/cmds/ls.bin', 'guest': '/bin/ls.bin'}])
    check(any('0 個' in x for x in p), "タグの無い行を報告する")

    p = problems_of(fake_plan(), base + [
        {'host': 'userland/x.bin', 'guest': '/bin/x.bin', 'tags': ['core', 'test']}])
    check(any('2 個' in x for x in p), "2 つのパッケージに当たる行を報告する")

    p = problems_of(fake_plan(), base + [
        {'host': 'userland/tests/foo.bin', 'guest': '/usr/bin/foo.bin',
         'tags': ['programs']}])
    check(any('userland/tests/' in x and 'test' in x for x in p),
          "userland/tests/ 由来が programs なのを報告する")

    p = problems_of(fake_plan(extra_pkg={'extra': {'tags': ['core']}}), base)
    check(any('2 つのパッケージ' in x for x in p), "1 つのタグを 2 つのパッケージが取るのを報告する")

    p = problems_of(fake_plan(exclude=[{'guest': '/sys/shell.bin'}]), base)
    check(any('理由' in x for x in p), "理由の無い除外を報告する")

    p = problems_of(fake_plan(exclude=[{'guest': '/nope', 'reason': 'x'}]), base)
    check(any('古い除外' in x for x in p), "どれにも当たらない除外を報告する")

    plan = fake_plan(exclude=[{'guest': '/sys/shell.bin', 'reason': '試験'}])
    pkgs, p = mkpkg.plan_packages(plan, ROOT, fake_manifest(base))
    inside = [g for _, _, fs in pkgs for g, _ in fs]
    check(p == [] and '/sys/shell.bin' not in inside,
          "理由つきの除外はどのパッケージにも入らず、問題にもならない")

    p = problems_of(fake_plan(), base + [
        {'host': 'userland/sh.bin', 'guest': '/sys/shell.bin', 'tags': ['programs']}])
    check(any('2 回' in x for x in p), "ゲストパスの重複を報告する")

    p = problems_of(fake_plan(), base, loader='boot/other.bin')
    check(any('boot.loader' in x for x in p), "BOOT が配備の boot.loader を含まないのを報告する")

    p = problems_of(fake_plan(extra_pkg={'toolong8': {'tags': ['x']}}), base)
    check(any('文字を超える' in x for x in p), "8 文字の名前を報告する (連番の余地)")

    # glob の行: exclude: のベース名は配備と同じく外れる
    g = [{'host': 'userland/cmds/*.bin', 'guest': '/bin/', 'type': 'glob',
          'exclude': ['more.bin'], 'tags': ['programs']}]
    pkgs, p = mkpkg.plan_packages(fake_plan(), ROOT, fake_manifest(base + g))
    inside = [gp for _, _, fs in pkgs for gp, _ in fs]
    check('/bin/more.bin' not in inside and '/bin/less.bin' in inside,
          "glob の exclude: は配備と同じく外れる")


# ---------------------------------------------------------------- 3. 分割

def case_split():
    print("case 3: 分割")
    files = [(f"/usr/man/p{i:03d}.1", "x") for i in range(200)] + \
            [(f"/bin/c{i:03d}.bin", "x") for i in range(60)]
    parts = mkpkg.split_files('normal', files)
    names = [n for n, _ in parts]
    check(names == ['normal', 'normal2', 'normal3'], f"名前 {names}")
    ok = all(len(fs) + len(mkpkg.entry_dirs(g for g, _ in fs)) <= mkpkg.PKG_MAX_ENTRIES
             for _, fs in parts)
    check(ok, "どの分割も 128 項目以下")
    check([f for _, fs in parts for f in fs] == files, "順序を保ち、欠けも重複も無い")
    parts = mkpkg.split_files('minimal', files[:10])
    check([n for n, _ in parts] == ['minimal'], "小さければ分けない")
    # ちょうど 128 (126 ファイル + /usr と /usr/man)
    exact = [(f"/usr/man/q{i:03d}.1", "x") for i in range(126)]
    check(len(mkpkg.split_files('n', exact)) == 1, "ちょうど 128 項目は 1 本")
    check(len(mkpkg.split_files('n', exact + [("/usr/man/z.1", "x")])) == 2,
          "129 項目で 2 本")
    # 読み手 (read_pkg) の LZSS 展開が書き手と往復する
    tmp = tempfile.mkdtemp(prefix='pkgrt_')
    try:
        a = os.path.join(tmp, 'a')
        b = os.path.join(tmp, 'b')
        open(a, 'wb').write(bytes((i * 7) & 0xFF for i in range(9000)) + b'abc' * 3000)
        open(b, 'wb').write(os.urandom(5000))
        blob = mkpkg.build_pkg('rt', 1, [('/x/a', a), ('/b', b)], True, 0)
        out = os.path.join(tmp, 'RT.PKG')
        open(out, 'wb').write(blob)
        hdr, ents = mkpkg.read_pkg(out)
        files = {g: d for g, t, d in ents if t == mkpkg.PKG_TYPE_FILE}
        check(hdr['flags'] & mkpkg.PKG_FLAG_LZSS and
              files == {'/x/a': open(a, 'rb').read(), '/b': open(b, 'rb').read()},
              "LZSS の PKG を読み戻すと元のバイト列")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------- 4/5. 媒体

def expected_files(plan):
    """[(guest, host_abs)] — 配備マニフェスト (除外以外) + 構成に直に書いた物"""
    excl = {e['guest'] for e in plan.get('exclude') or []}
    want = {}
    man = dm.load_merged(dm.CORE_MANIFEST_RELPATHS)
    for e in man['filesystem']['files']:
        for host, guest in dm.resolve_entry(e, ROOT):
            if guest not in excl:
                want[guest] = os.path.join(ROOT, host)
    for pdef in plan['packages'].values():
        for f in (pdef or {}).get('files') or []:
            want[f['guest']] = os.path.join(ROOT, f['host'])
    return want


def verify_media(pkg_paths, want, label):
    """PKG 群を読み戻し、want とちょうど一致するか"""
    got = {}
    dup = []
    bad_bytes = []
    ok = True
    for path in pkg_paths:
        try:
            hdr, entries = mkpkg.read_pkg(path)
        except ValueError as e:
            check(False, f"{label}: {os.path.basename(path)} が読めない: {e}")
            ok = False
            continue
        if hdr['entry_count'] > mkpkg.PKG_MAX_ENTRIES:
            check(False, f"{label}: {os.path.basename(path)} {hdr['entry_count']} 項目")
            ok = False
        for g, t, data in entries:
            if t != mkpkg.PKG_TYPE_FILE:
                continue
            if g in got:
                dup.append(g)
            got[g] = os.path.basename(path)
            h = want.get(g)
            if h is not None:
                with open(h, 'rb') as f:
                    if f.read() != data:
                        bad_bytes.append(g)
    missing = sorted(set(want) - set(got))
    extra = sorted(set(got) - set(want))
    for g in missing[:20]:
        print(f"       missing {g}")
    for g in extra[:20]:
        print(f"       extra   {g}")
    for g in bad_bytes[:20]:
        print(f"       bytes   {g}")
    ok &= check(not missing, f"{label}: 配備の全ファイルが入っている ({len(want)} 件、欠け {len(missing)})")
    ok &= check(not extra, f"{label}: 余分な物が無い (余分 {len(extra)})")
    ok &= check(not dup, f"{label}: 重複が無い ({len(dup)})")
    ok &= check(not bad_bytes, f"{label}: 中身が同じバイト列 (違い {len(bad_bytes)})")
    return ok


def case_media(plan, resolved):
    print("case 4: 実物の mkpkg で作った PKG を読み戻す")
    want = expected_files(plan)
    missing_art = [h for h in want.values() if not os.path.isfile(h)]
    if missing_art:
        for h in missing_art[:10]:
            print(f"       成果物が無い: {os.path.relpath(h, ROOT)}")
        check(False, "成果物が揃っている (先に make all)")
        return None
    tmp = tempfile.mkdtemp(prefix='pkgtest_')
    try:
        # 古い PKG が消えることも見る
        open(os.path.join(tmp, 'FULL.PKG'), 'wb').write(b'stale')
        r = subprocess.run([sys.executable, '-B', os.path.join(ROOT, 'tools', 'mkpkg.py'),
                            '--plan', PLAN, '--output', tmp, '--base', ROOT],
                           capture_output=True, text=True)
        check(r.returncode == 0, f"mkpkg --plan が通る (rc={r.returncode})")
        if r.returncode != 0:
            print(r.stderr)
            return None
        names = sorted(os.listdir(tmp))
        check('FULL.PKG' not in names, "出力先の古い PKG を消す")
        check(names == sorted(n.upper() + '.PKG' for n, _, _, _ in resolved),
              f"PKG の顔ぶれ {names}")
        verify_media([os.path.join(tmp, n) for n in names if n != 'BOOT.PKG'],
                     {g: h for g, h in want.items()
                      if g not in {f['guest'] for f in plan['packages']['boot']['files']}},
                     "packages")
        hdr, _ = mkpkg.read_pkg(os.path.join(tmp, 'BOOT.PKG'))
        check(hdr['flags'] == 0, "BOOT.PKG は無圧縮 (cdinst が断る)")
        return {n: open(os.path.join(tmp, n), 'rb').read() for n in names}
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def case_iso(plan, fresh):
    print("case 5: ISO の中の PKG")
    isoinfo = shutil.which('isoinfo')
    if not isoinfo:
        print("  SKIP isoinfo が無い (genisoimage パッケージ)")
        return
    if not os.path.isfile(ISO):
        check(False, f"{os.path.relpath(ISO, ROOT)} がある (先に make all)")
        return
    r = subprocess.run([isoinfo, '-f', '-i', ISO], capture_output=True, text=True)
    names = sorted(l.strip().lstrip('/').split(';')[0] for l in r.stdout.splitlines()
                   if l.strip())
    check(fresh is not None and names == sorted(fresh),
          f"ISO の顔ぶれが今の構成と同じ {names} (違えば make iso)")
    tmp = tempfile.mkdtemp(prefix='isotest_')
    try:
        paths = []
        for n in names:
            out = os.path.join(tmp, n)
            with open(out, 'wb') as f:
                subprocess.run([isoinfo, '-i', ISO, '-x', f'/{n};1'], stdout=f, check=True)
            paths.append(out)
            if fresh and n in fresh:
                check(open(out, 'rb').read() == fresh[n],
                      f"ISO の {n} が今作った物と同じバイト列 (違えば make iso)")
        boot = {f['guest'] for f in plan['packages']['boot']['files']}
        want = {g: h for g, h in expected_files(plan).items() if g not in boot}
        verify_media([p for p in paths if not p.endswith('BOOT.PKG')], want, "ISO")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------- 6. 消費側

def case_consumer(plan):
    print("case 6: cdinst.c / pkg.h との突き合わせ")
    src = open(CDINST, encoding='utf-8').read()
    bases = set(re.findall(r'#define\s+PKG_BASE_\w+\s+"(\w+)"', src))
    want = {n.upper() for n, p in plan['packages'].items()
            if (p or {}).get('type') != 'boot'}
    check(bases == want, f"cdinst のベース名 {sorted(bases)} = 構成 {sorted(want)}")
    check('BOOT.PKG' in src, "cdinst は BOOT.PKG を読む")
    seq = [m.group(1) for m in re.finditer(r'install_series\(PKG_BASE_(\w+)\)', src)]
    check(seq == ['MINIMAL', 'GUI', 'NORMAL', 'DEBUG'],
          f"cdinst の展開順 {seq} = MINIMAL → GUI → NORMAL → DEBUG")
    check('FULL.PKG' not in src and 'APPEND.PKG' not in src,
          "cdinst に古い FULL / APPEND が残っていない")
    m = re.search(r'#define\s+PKG_SERIES_MAX\s+(\d+)', src)
    check(m is not None and int(m.group(1)) == 9, "cdinst の連番上限 9 = mkpkg の上限")
    h = open(PKG_H, encoding='utf-8').read()
    mp = re.search(r'#define\s+PKG_MAX_PATH\s+(\d+)', h)
    me = re.search(r'#define\s+PKG_MAX_ENTRIES\s+(\d+)', h)
    check(mp and int(mp.group(1)) == mkpkg.PKG_MAX_PATH, "PKG_MAX_PATH が一致")
    check(me and int(me.group(1)) == mkpkg.PKG_MAX_ENTRIES, "PKG_MAX_ENTRIES が一致")


# ---------------------------------------------------------------- 7. 起動 FD

def fat12_files(path):
    """FAT12 イメージのファイルを {PATH (大文字): bytes} と空きバイトで返す

    BPB (オフセット 0x0B) から配置を読む。mkfat12 が作る 2HD (1024B/セクタ) と
    1.44MB (512B/セクタ) の両方。. / .. と削除済みは飛ばす。
    """
    img = open(path, 'rb').read()
    import struct
    bps, spc, rsv, nfat, nroot, tot, _media, fatsz = \
        struct.unpack_from('<HBHBHHBH', img, 0x0B)
    fat = img[rsv * bps:(rsv + fatsz) * bps]
    root_off = (rsv + nfat * fatsz) * bps
    root_secs = (nroot * 32 + bps - 1) // bps
    data_sec = rsv + nfat * fatsz + root_secs
    nclus = (tot - data_sec) // spc
    csize = bps * spc

    def nxt(c):
        v = fat[c * 3 // 2] | (fat[c * 3 // 2 + 1] << 8)
        return (v >> 4) if c & 1 else (v & 0xFFF)

    def chain(c):
        out = []
        while 2 <= c < 0xFF8 and len(out) <= nclus:
            out.append(c)
            c = nxt(c)
        return out

    def read_chain(c):
        return b''.join(img[(data_sec + (x - 2) * spc) * bps:][:csize] for x in chain(c))

    files = {}

    def walk(raw, prefix):
        for i in range(0, len(raw) - 31, 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F or e[0] == ord('.'):
                continue
            base = e[0:8].decode('ascii').rstrip()
            ext = e[8:11].decode('ascii').rstrip()
            name = base + ('.' + ext if ext else '')
            clus = struct.unpack_from('<H', e, 26)[0]
            size = struct.unpack_from('<I', e, 28)[0]
            if e[11] & 0x10:
                walk(read_chain(clus), prefix + '/' + name)
            elif not e[11] & 0x08:
                files[prefix + '/' + name] = read_chain(clus)[:size] if size else b''

    walk(img[root_off:root_off + nroot * 32], '')
    used = sum(1 for c in range(2, nclus + 2) if nxt(c) != 0)
    return files, (nclus - used) * csize, nclus * csize


def case_fd(plan, resolved):
    print("case 7: 起動 FD = BOOT + MINIMAL (+ fd.only)")
    mk = open(IMAGE_MK, encoding='utf-8').read()
    check(not re.search(r'^FDD_MIN_CMDS\s*[:?]?=', mk, re.M) and '--fd-args' in mk
          and '/bin/$$cmd.bin' not in mk,
          "build/image.mk は FD の一覧を手で持たず mkpkg --fd-args から作る")
    fd = plan.get('fd') or {}
    rename = {r['guest']: r['fd'] for r in fd.get('rename') or []}
    only = {o['fd'] for o in fd.get('only') or []}
    check(fd.get('from') == ['boot', 'minimal'], f"FD の元は BOOT + MINIMAL ({fd.get('from')})")

    # 構成: FD の中身 (rename を戻し、only を除いた集合) = BOOT + MINIMAL
    src = {g: os.path.join(ROOT, h) for n, _, _, fs in resolved
           if n == 'boot' or n.startswith('minimal') for g, h in fs}
    plans = {}
    for label, _, loader in FD_IMAGES:
        files, probs = mkpkg.fd_plan(plan, ROOT, loader)
        for p in probs:
            print(f"       {p}")
        check(probs == [], f"FD {label}: fd_plan に問題が無い")
        back = {v: k for k, v in rename.items()}
        core = {back.get(g, g) for g, _ in files if g not in only}
        check(core == set(src),
              f"FD {label}: 中身 = BOOT + MINIMAL (FD にだけ {sorted(core - set(src))}、"
              f"MINIMAL にだけ {sorted(set(src) - core)})")
        check(sorted(g for g, _ in files if g in only) == sorted(only),
              f"FD {label}: FD だけの物は fd.only の {sorted(only)} だけ")
        check(files and files[0][0] == '/LOADER.BIN' and files[0][1] == loader,
              f"FD {label}: 先頭が LOADER.BIN ({loader})")
        plans[label] = files
    check(not any(g in ('/bin/timetest.bin', '/bin/pcmtest.bin') for g, _ in plans['2HD']),
          "FD に試験用の timetest / pcmtest が無い (DEBUG にある)")

    # 否定側
    base = [{'host': 'userland/shell.bin', 'guest': '/sys/shell.bin', 'tags': ['core']}]
    fp = fake_plan()
    fp['fd'] = {'from': ['boot', 'minimal'], 'rename': [], 'only': []}
    _, p = mkpkg.fd_plan(fp, ROOT, None, fake_manifest(base + [
        {'host': 'assets/filetypes', 'guest': '/etc/filetypes', 'tags': ['core']}]))
    check(any('8.3' in x for x in p), "否定: 8.3 に収まらない名前を報告する")
    fp['fd'] = {'from': ['boot', 'minimal'],
                'rename': [{'guest': '/nope', 'fd': '/NOPE', 'reason': 'x'}], 'only': []}
    _, p = mkpkg.fd_plan(fp, ROOT, None, fake_manifest(base))
    check(any('古い rename' in x for x in p), "否定: どれにも当たらない rename を報告する")
    fp['fd'] = {'from': ['boot', 'minimal'],
                'rename': [{'guest': '/sys/shell.bin', 'fd': '/sys/sh.bin'}], 'only': []}
    _, p = mkpkg.fd_plan(fp, ROOT, None, fake_manifest(base))
    check(any('理由' in x for x in p), "否定: 理由の無い rename を報告する")
    fp['fd'] = {'from': ['boot', 'minimal'], 'rename': [],
                'only': [{'fd': '/LOADER.BIN', 'host': '{loader}', 'reason': 'x'}]}
    _, p = mkpkg.fd_plan(fp, ROOT, None, fake_manifest(base))
    check(any('ローダ' in x for x in p), "否定: ローダの指定が無いのを報告する")
    fp['fd'] = {'from': ['boot', 'minimal'], 'rename': [], 'only': [
        {'fd': '/SYS/SHELL.BIN', 'host': 'x', 'reason': 'x'}]}
    _, p = mkpkg.fd_plan(fp, ROOT, None, fake_manifest(base))
    check(any('2 回' in x for x in p), "否定: FD のパスの重複 (大文字小文字を畳む) を報告する")

    # 実物のイメージ
    for label, img, _ in FD_IMAGES:
        if not os.path.isfile(img):
            check(False, f"{os.path.relpath(img, ROOT)} がある (先に make all)")
            continue
        got, free, total = fat12_files(img)
        want = {g.upper(): h for g, h in plans[label]}
        missing = sorted(set(want) - set(got))
        extra = sorted(set(got) - set(want))
        bad = sorted(g for g in set(want) & set(got)
                     if open(want[g], 'rb').read() != got[g])
        for g in missing[:10]:
            print(f"       missing {g}")
        for g in extra[:10]:
            print(f"       extra   {g}")
        for g in bad[:10]:
            print(f"       bytes   {g}")
        check(not missing and not extra and not bad,
              f"FD {label} の実物 = 構成 ({len(got)} ファイル、欠け {len(missing)}、"
              f"余分 {len(extra)}、違い {len(bad)})")
        print(f"  info FD {label}: 空き {free // 1024}KB / {total // 1024}KB")


def main():
    plan, resolved = case_real_plan()
    case_negative()
    case_split()
    fresh = case_media(plan, resolved)
    case_iso(plan, fresh)
    case_consumer(plan)
    case_fd(plan, resolved)
    print()
    if fails:
        print(f"FAIL {len(fails)} 件")
        return 1
    print("PASS")
    return 0


if __name__ == '__main__':
    sys.exit(main())
