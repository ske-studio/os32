"""変異試験を並列に回す共通部 (票 docs/tasks/tools/TASK_CHECK_MUT_PARALLEL.md)。

使う側 (test_hdd_stage2.py など) は変異 1 本を「自分専用の一時ディレクトリの写しで
組んで回し、結果を返す」関数にして run_ordered に渡す。結果は**変異の番号順**に
返る (Executor.map の順) ので、出力の順序と書式は逐次のときと変わらない。

並列度: 環境変数 OS32_MUT_JOBS (1 以上の整数。1 なら逐次)。既定は
os.cpu_count() // 2。make -j の下で 6 本の重い試験が同時に走っても CPU を
奪い合いすぎない値として実測で選んだ (票 §3)。変異を回すあいだはプロセスの
優先度を下げる (OS32_MUT_NICE、既定 +10)。

作り直しの表の検査 (check_rebuild_table): 各試験は「変異が当たったファイル →
組み直すハーネス」の表を持つ。表が gcc -MM の依存より狭いと、変異を当てたのに
組み直さない (= 見逃す) ので、変異の前に表を依存と突き合わせ、狭ければ試験を落とす。
表に無いファイルはハーネスを全部組む (安全側)。
"""
import concurrent.futures
import os
import pathlib
import re
import shutil
import subprocess

JOBS_ENV = "OS32_MUT_JOBS"
NICE_ENV = "OS32_MUT_NICE"
NICE_DEFAULT = 10
_niced = False


def jobs():
    """並列度。OS32_MUT_JOBS が 1 以上の整数ならそれ、でなければ CPU 数の半分。"""
    v = os.environ.get(JOBS_ENV, "").strip()
    if v:
        try:
            n = int(v)
        except ValueError:
            n = 0
        if n >= 1:
            return n
    return max(1, (os.cpu_count() or 2) // 2)


def _lower_priority():
    """変異を並べる前に 1 回だけ、このプロセスの優先度を下げる (既定 nice +10、
    OS32_MUT_NICE で変える、0 なら下げない)。スレッドも子プロセス (gcc・ハーネス) も
    これを受け継ぐ。変異は後回しにしてよいバッチの仕事で、make check の同じ段には
    時間に敏感な試験 (test_lan_bridge.py の再入の競合など) が並んでいる — 変異が CPU を
    埋めると、それらが競合を踏めずに見逃しになった (2026-09-26、票 §4)。"""
    global _niced
    if _niced:
        return
    _niced = True
    try:
        n = int(os.environ.get(NICE_ENV, NICE_DEFAULT))
    except ValueError:
        n = NICE_DEFAULT
    if n > 0:
        try:
            os.nice(n)
        except OSError:
            pass


def run_ordered(fn, items, processes=False):
    """fn(item) を並列に回し、結果を items の順に yield する。

    既定はスレッド (中身が gcc と実行ファイルの subprocess のとき。GIL を離して
    待つので足りる)。変異 1 本の中で Python の計算が重いもの (像を組む・壊す、
    Python の実物を読み込み直して回す) は processes=True — スレッドでは GIL で
    詰まって並列にならない (test_vk32_crc.py で実測、票 §3)。processes のとき
    fn は試験スクリプトの最上位の関数、items と結果は pickle できるものにする。
    fn は作業ツリーの実物にも、ほかの変異の写しにも書かないこと。"""
    items = list(items)
    n = min(jobs(), max(1, len(items)))
    _lower_priority()
    if n == 1:
        for it in items:
            yield fn(it)
        return
    pool = (concurrent.futures.ProcessPoolExecutor if processes
            else concurrent.futures.ThreadPoolExecutor)
    with pool(max_workers=n) as ex:
        for r in ex.map(fn, items):
            yield r


def gcc_deps(cmd, root, cwd=None):
    """gcc のコマンド (…, -o exe) を -MM に替えて流し、依存するファイルを
    root からの相対パスの集合で返す (root の外は捨てる)。"""
    root = pathlib.Path(root).resolve()
    args = []
    skip = False
    for a in cmd:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a in ("-c", "-S", "-E"):
            continue
        args.append(a)
    r = subprocess.run(args + ["-MM"], cwd=cwd or str(root), capture_output=True,
                       text=True, check=True)
    deps = set()
    for tok in r.stdout.replace("\\\n", " ").split():
        if tok.endswith(":"):
            continue
        p = pathlib.Path(os.path.normpath(os.path.join(cwd or str(root), tok)))
        try:
            deps.add(str(p.relative_to(root)))
        except ValueError:
            pass
    return deps


def check_rebuild_table(table, deps_by_harness, files):
    """表 {ファイル: {ハーネス…}} が依存 {ハーネス: {ファイル…}} を覆っているか。

    files (変異を当てるファイル) のうち表にあるものについて、そのファイルに
    依存するハーネスが表の集合に入っていなければ問題として返す。"""
    bad = []
    for f in sorted(set(files)):
        if f not in table:
            continue
        need = {h for h, d in deps_by_harness.items() if f in d}
        miss = need - set(table[f])
        if miss:
            bad.append("{}: 表 {} に {} が無い (gcc -MM)".format(
                f, sorted(table[f]), sorted(miss)))
    return bad


# ---------------------------------------------------------------- 写しの木
# 実物のソースに変異を当てて戻す作り (check-mut の -j1 の段) をやめるための道具
# (票 §5)。変異 1 本ごとに一時ディレクトリへ「作業ツリーの写し」を作り、
# 変異はその写しにだけ当てる。写しは安く作る: 変異を当てるファイルと、その
# 祖先のディレクトリ・gcc -MM の依存 (real) だけを実体で複写し、それ以外の項目は
# 実物への symlink にする。
#
# 実体にしたディレクトリの中の "..." の #include は写しの中で解決される。
# symlink のディレクトリの下のファイルは実物を指すので、そこから `..` で
# 辿る #include は実物へ出る — だから C の試験は gcc -MM の依存を全部 real に
# 入れる (依存が全部実体なら、依存を含むディレクトリも全部実体になる)。
# 写しの symlink を通して書くと実物を書き換える。試験が写しの中で書くファイルは
# real に入れること (tools/check_tree_unchanged.py の番人が残りを捕まえる)。

def overlay(root, dst, real=()):
    """dst に root の写しを作って dst (pathlib.Path) を返す。

    real (root からの相対パス、ファイルでもディレクトリでもよい) は実体で
    複写する。それ以外は、real の祖先のディレクトリの各項目を root への
    symlink にする。"""
    root = pathlib.Path(root).resolve()
    dst = pathlib.Path(dst)
    rels = sorted({os.path.normpath(r) for r in real if r})
    for r in rels:
        if r.startswith("..") or os.path.isabs(r):
            raise ValueError("overlay: root の外: %s" % r)
    # 実体にするディレクトリの下にあるものは、そのディレクトリの複写に含まれる
    rdirs = {r for r in rels if (root / r).is_dir()}

    def under_rdir(r):
        p = os.path.dirname(r)
        while p:
            if p in rdirs:
                return True
            p = os.path.dirname(p)
        return False
    rels = [r for r in rels if not under_rdir(r)]
    anc = {"."}
    for r in rels:
        p = os.path.dirname(r)
        while p:
            anc.add(p)
            p = os.path.dirname(p)
    realset = set(rels)
    dst.mkdir(parents=True, exist_ok=True)
    for d in sorted(anc, key=lambda x: (x.count(os.sep), x)):
        (dst / d).mkdir(parents=True, exist_ok=True)
        for ent in os.listdir(root / d):
            rel = os.path.normpath(os.path.join(d, ent))
            if rel in anc or rel in realset:
                continue
            os.symlink(root / rel, dst / rel)
    for r in rels:
        src = root / r
        if src.is_dir():
            shutil.copytree(src, dst / r, symlinks=True)
        elif src.exists():
            shutil.copy2(src, dst / r)
    return dst


def rebase(cmd, root, tree):
    """コマンドの引数に現れる root の絶対パスを tree に替える (-I<root>/x も)。"""
    pat = re.compile(re.escape(str(pathlib.Path(root).resolve())) + r"(?=/|$)")
    return [pat.sub(lambda _m: str(tree), str(a)) for a in cmd]


def mutant_tree(root, dst, edits, real=(), gcc_cmds=()):
    """変異を当てた写しの木を dst に作って返す。

    edits は {相対パス: 変異後の中身 (str)}。gcc_cmds (root に対して組んだ
    gcc のコマンドの並び、cwd=root) の gcc -MM の依存も実体で複写する。"""
    real = set(real) | set(edits)
    for cmd in gcc_cmds:
        real |= gcc_deps(cmd, root)
    tree = overlay(root, dst, real)
    for rel, text in edits.items():
        p = tree / rel
        if p.is_symlink():
            raise RuntimeError("mutant_tree: %s が symlink のまま" % rel)
        p.write_text(text, encoding="utf-8")
    return tree


def build_in_tree(root, td, edits, cmds, real=(), **kw):
    """edits を当てた写しの木を td/tree に作り、cmds (root に対して組んだ
    コマンドの並び。実物の木で cwd=root で流すのと同じ形) を写しの中で順に流す。

    gcc 系のコマンドは gcc -MM の依存を実体で複写してから流す。コマンドが
    落ちたら subprocess.CalledProcessError をそのまま上げる。kw は
    subprocess.run へ渡す (capture_output など)。写しの木を返す。"""
    gccs = [c for c in cmds if os.path.basename(str(c[0])).endswith("gcc")]
    tree = mutant_tree(root, pathlib.Path(td) / "tree", edits, real=real,
                       gcc_cmds=gccs)
    for c in cmds:
        subprocess.run(rebase(c, root, tree), cwd=str(tree), check=True, **kw)
    return tree


def run_script_in_tree(root, td, edits, script, args=(), real=(), **kw):
    """Python の試験を「変異を当てた写し」の上で流し直す (実物の Python を
    import して試す試験の否定側)。script (root からの相対) も実体で複写するので、
    その中の `ROOT = pathlib.Path(__file__).resolve().parents[2]` は写しを指す。
    subprocess.run の結果を返す (kw はそのまま渡す)。"""
    import sys
    tree = mutant_tree(root, pathlib.Path(td) / "tree", edits,
                       real=set(real) | {script})
    return subprocess.run([sys.executable, "-B", str(tree / script)] + list(args),
                          cwd=str(tree), **kw)


def run_with_control(fn, items, control, out=None):
    """変異を並列に回し、(印字, 見逃し) を順に印字して見逃しの数を返す。

    control は「変異なし」の項目 (old == new の変異)。変異と同じ fn で写しの木を
    作って回し、**GREEN であること**を確かめる — 写しの作りが壊れて (依存の
    複写漏れでコンパイルが通らない、写しの中で入力が見つからない) どの変異も
    別の理由で RED になるのを見逃さないため。fn(control) は生き残りとして
    (「GREEN のまま」を含む行, 1) を返すはずで、それ以外なら 1 を足す。"""
    import sys
    out = out or sys.stdout
    res = list(run_ordered(fn, [control] + list(items)))
    ctext, cbad = res[0]
    bad = 0
    if cbad == 1 and "GREEN" in ctext:
        out.write("CONTROL 変異なしの写しの木 GREEN (期待どおり)\n")
    else:
        out.write("CONTROL 変異なしの写しの木が通らない = 写しの作りが壊れている: "
                  "%s\n" % ctext.strip().splitlines()[-1])
        bad += 1
    out.flush()
    for text, b in res[1:]:
        out.write(text + "\n")
        out.flush()
        bad += b
    return bad
