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
