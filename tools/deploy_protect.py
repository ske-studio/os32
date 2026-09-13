#!/usr/bin/env python3
"""deploy_protect.py — 通常配備が /etc/settings.db* を壊さないための共通判定 (票 S0-D / D0)

契約の正典: docs/tasks/settings/S0_FOUNDATION.md §5、手順は docs/tasks/settings/TASK_S0.md §2。

設定レジストリ (`/etc/settings.db`) は**ゲストが書く**もので、ホストのビルド成果物では
ない。ところが配備ツールは「マニフェストにある物を書く」「HostDrv の中身をそのまま
NHD へ写す」「マニフェストに無い物を消す」の 3 系統があり、どこか 1 つでも settings.db
を掴むとユーザーの設定が消える。ここは**書く / 消す / 切り詰める直前**に 1 か所で止める
ための判定だけを置く。新しい配備 framework は作らない。

判定は 2 段 (Codex 往復 2 の 6):

  (1) 名前規則 — realpath の**前**に、字句正規化したゲスト名の親が /etc で
      basename が保護対象名なら真。実体が欠損していても (dangling symlink、
      `etc -> conf` のような別名) 「欠損は欠損のまま」守れる。
  (2) 実体規則 — realpath で解決し、root の外なら拒否、解決先の st_dev/st_ino が
      **存在する** <root>/etc/settings.db* のいずれかと一致すれば真 (hardlink /
      symlink の別名対策)。解決後のゲスト名にも (1) を当てる。

`stat` の失敗は **ENOENT (確定した不存在) だけ「保護対象ではない」**とする。初回の
`/etc/settings.tsv` のような新規ファイルを配備できるのはこのため。それ以外
(EACCES / EIO / ELOOP / 解決失敗) は保護側に倒し、ProtectError で**配備全体を失敗**
させる (往復 3 の 3)。判断できないまま書くほうが危ない。

`<root>/etc` 自体が symlink または別マウントなら配備全体を拒否する。bind mount の
別名はツールでは検出しきれないので運用で禁止し、ここで最低限止める。

標準ライブラリのみ。実配備・sudo・mount は一切行わない (判定と印字だけ)。
"""

import errno
import os
import stat
import sys

# 保護対象のベース名 (大文字小文字を区別しない)。
# WAL / SHM は OS32 が WAL を使う意味ではなく、安全側の巻き取り。
PROTECTED_BASENAMES = frozenset([
    'settings.db',
    'settings.db-journal',
    'settings.db-wal',
    'settings.db-shm',
    'settings.db.bak',
])

# 保護対象ディレクトリ (ゲストの絶対パス、小文字)
PROTECTED_DIR = '/etc'


class ProtectError(Exception):
    """保護の判定そのものができなかった / 迂回が見つかった。配備を失敗させる。"""


class ProtectedPath(ProtectError):
    """その道に保護対象が居るので**作らない / 書かない**。

    ProtectError を継承しているので「保護の系が投げた」ことは 1 つの except で
    受けられるが、呼び出し側は**除外 (成功、ログだけ)** と「判定できない (失敗)」
    を区別すること (票 §6 往復 1 の B2 / B7)。
    """

    def __init__(self, guest, message=None):
        ProtectError.__init__(
            self, message or '保護対象なので作らない: {}'.format(guest))
        self.guest = guest


# ---------------------------------------------------------------- 字句正規化

def normalize_guest_path(guest_path, host_src=None):
    """ゲストパスを字句正規化して先頭 '/' 付きの絶対パスにする。

    - `'/etc/'` のようなディレクトリ指定は host_src の basename を補う。
    - `'.'` / 連続 `'/'` を畳み、`'..'` は 1 段戻す。root を越えるものは拒否。
    OS には一切触らない (symlink を辿らない = realpath より前に使える)。
    """
    if guest_path is None:
        raise ProtectError('ゲストパスが None')
    p = str(guest_path)
    # `guest: /etc/` のようなディレクトリ指定は、ソースがあれば basename を補う。
    # ソースが無い (mkdir の判定など) ならディレクトリそのものとして扱う。
    if (p.endswith('/') or p == '') and host_src is not None:
        p = p + os.path.basename(host_src)

    parts = []
    for seg in p.split('/'):
        if seg == '' or seg == '.':
            continue
        if seg == '..':
            if not parts:
                raise ProtectError(
                    'root の外へ出るゲストパス: {!r}'.format(guest_path))
            parts.pop()
            continue
        parts.append(seg)
    # `/bin/..` のように root へ戻り切るのは**正当**に '/' (往復 1 の B9)。
    # 拒否するのは root より上へ出る場合だけ (上の '..' の分岐)。
    if not parts:
        return '/'
    return '/' + '/'.join(parts)


def name_is_protected(guest_path):
    """字句正規化済みのゲストパスに**名前規則**だけを当てる (OS に触らない)。"""
    head, _, base = guest_path.rpartition('/')
    if not head:
        head = '/'
    return head.lower() == PROTECTED_DIR and base.lower() in PROTECTED_BASENAMES


# ------------------------------------------------------------ 最終パスの確定

def _inside(root_abs, path):
    return path == root_abs or path.startswith(root_abs + os.sep)


def resolve_dest(root, guest_path, host_src=None):
    """**実際に操作する最終ファイル名**をホスト側の絶対パスで確定する。

    root は配備先のツリー (マウントした NHD / HostDrv のルート)。
    root の外へ出る結果は ProtectError。

    字句の補完 (`guest: /etc/`) だけでは足りない: `cp SRC /etc` も
    `shutil.copy2(SRC, '/etc')` も、**宛先が既存ディレクトリなら中へ**書く。
    `--dest / --rename etc` に `settings.db` という名前のソースを渡すと、字句上の
    宛先 `/etc` は保護対象ではないのに実体は `/etc/settings.db` になる
    (往復 1 の B1)。ここで実ディレクトリを見て basename を補い、呼び出し側は
    **この戻り値だけ**を cp / copy2 に渡すこと (ディレクトリを渡さない)。
    """
    guest = normalize_guest_path(guest_path, host_src)
    root_abs = os.path.abspath(root)
    dest_abs = os.path.abspath(os.path.join(root_abs, guest.lstrip('/')))
    if not _inside(root_abs, dest_abs):
        raise ProtectError(
            '配備先 {!r} が root {!r} の外にある'.format(dest_abs, root_abs))
    # symlink 越しのディレクトリでも cp / copy2 は中へ書くので isdir で見る
    # (辿った先が root の外なら下の検査か is_protected の realpath が止める)。
    if host_src is not None and os.path.isdir(dest_abs):
        dest_abs = os.path.join(dest_abs, os.path.basename(host_src))
        if not _inside(root_abs, dest_abs):
            raise ProtectError(
                '配備先 {!r} が root {!r} の外にある'.format(dest_abs, root_abs))
    return dest_abs


def guest_path_of(root, dest):
    """root 配下のホストパスからゲストの絶対パスを逆算する。"""
    root_abs = os.path.abspath(root)
    dest_abs = os.path.abspath(dest)
    if dest_abs == root_abs:
        return '/'
    if not dest_abs.startswith(root_abs + os.sep):
        raise ProtectError(
            '{!r} が root {!r} の外にある'.format(dest_abs, root_abs))
    rel = dest_abs[len(root_abs) + 1:]
    return '/' + rel.replace(os.sep, '/')


# -------------------------------------------------------------------- 実体側

def _stat_or_none(path):
    """os.stat。ENOENT だけ None、他の失敗は ProtectError (保護側に倒す)。"""
    try:
        return os.stat(path)
    except OSError as exc:
        if exc.errno == errno.ENOENT:
            return None
        raise ProtectError(
            '{} を stat できない ({})'.format(path, exc.strerror))


def check_root_etc(root):
    """<root>/etc が素のディレクトリで root と同じマウントにあることを確かめる。

    symlink / 別マウント / 通常ファイルなら ProtectError = **配備全体を拒否**。
    別名の bind mount は検出できないので運用で禁止する (TASK_S0 §2)。

    **全操作の前提検査**。名前規則だけで答えが出る経路 (`rm /etc/settings.db`
    など) でも必ず先に通す — `/etc` がすり替わっていれば、その配備は
    「保護対象かどうか」以前に信用できない (往復 1 の B4)。
    """
    root_abs = os.path.abspath(root)
    real_root = os.path.realpath(root_abs)
    etc = os.path.join(real_root, 'etc')
    if os.path.islink(etc):
        raise ProtectError('{} が symlink。配備を中止する'.format(etc))
    st_etc = _stat_or_none(etc)
    if st_etc is None:
        return
    if not stat.S_ISDIR(st_etc.st_mode):
        raise ProtectError(
            '{} がディレクトリではない。配備を中止する'.format(etc))
    if os.path.realpath(etc) != etc:
        raise ProtectError('{} が字句と異なる実体を指す。配備を中止する'.format(etc))
    st_root = _stat_or_none(real_root)
    if st_root is None:
        raise ProtectError('{} が消えた'.format(real_root))
    if st_etc.st_dev != st_root.st_dev:
        raise ProtectError(
            '{} が root と別のマウント。配備を中止する'.format(etc))


def _protected_identities(root):
    """<root>/etc にある**存在する**保護対象の (st_dev, st_ino) 一覧。"""
    real_root = os.path.realpath(os.path.abspath(root))
    etc = os.path.join(real_root, 'etc')
    try:
        names = os.listdir(etc)
    except OSError as exc:
        if exc.errno == errno.ENOENT:
            return []                   # 確定した不存在だけ「無い」とみなす
        # ENOTDIR / EACCES / EIO — 判断できないまま書くほうが危ない (往復 1 の B4)
        raise ProtectError('{} を読めない ({})'.format(etc, exc.strerror))
    ids = []
    for name in names:
        if name.lower() not in PROTECTED_BASENAMES:
            continue
        st = _stat_or_none(os.path.join(etc, name))
        if st is not None:
            ids.append((st.st_dev, st.st_ino))
    return ids


def is_protected(root, dest):
    """dest (ホスト側の最終パス) が保護対象かを 2 段で判定する。

    作成 / 切り詰め / 削除 / rename の**直前**に、確定した最終パスで呼ぶこと。
    判定できない状況は ProtectError (= 配備を失敗させる)。
    """
    # --- (0) 前提検査: <root>/etc がすり替わっていないか。名前規則で即答できる
    #         経路 (rm /etc/settings.db 等) でも必ず通す (往復 1 の B4)。
    check_root_etc(root)

    guest = guest_path_of(root, dest)

    # --- (1) 名前規則: realpath より前。実体が無くても守る。
    if name_is_protected(guest):
        return True

    # --- (2) 実体規則
    real_root = os.path.realpath(os.path.abspath(root))
    try:
        real_dest = os.path.realpath(os.path.abspath(dest))
    except OSError as exc:                                  # pragma: no cover
        raise ProtectError('{} を解決できない ({})'.format(dest, exc))
    if real_dest != real_root and not real_dest.startswith(real_root + os.sep):
        raise ProtectError(
            '{} は解決すると root {} の外を指す'.format(dest, real_root))

    # 解決後のゲスト名にも名前規則を当てる (symlink 別名で保護対象を作る道を塞ぐ)。
    if name_is_protected(guest_path_of(real_root, real_dest)):
        return True

    st = _stat_or_none(dest)
    if st is None:
        return False                    # 確定した不存在 = 新規の通常ファイル
    ident = (st.st_dev, st.st_ino)
    return ident in _protected_identities(root)


def mkdir_chain(root, guest_dir):
    """`mkdir -p` / `os.makedirs` が作る**各祖先**を root から順に確定する。

    `-p` / `makedirs` は途中のディレクトリを黙って作るので、最終要素だけを見ても
    `/etc/settings.db/a` の `settings.db` がディレクトリとして生える
    (往復 1 の B2)。root から 1 段ずつ判定し、保護対象に当たったら
    ProtectedPath を投げる (= その道は作らない。除外なので呼び出し側は成功扱い、
    往復 1 の B7)。

    Returns: 作るべき絶対パスの一覧 (root 自身は含まない、上から順)。
    """
    check_root_etc(root)
    guest = normalize_guest_path(guest_dir)
    root_abs = os.path.abspath(root)
    chain = []
    cur = root_abs
    for seg in guest.strip('/').split('/'):
        if not seg:
            continue
        cur = os.path.join(cur, seg)
        if not _inside(root_abs, cur):
            raise ProtectError(
                'ディレクトリ {!r} が root {!r} の外にある'.format(cur, root_abs))
        if is_protected(root_abs, cur):
            raise ProtectedPath(guest_path_of(root_abs, cur))
        chain.append(cur)
    return chain


def is_protected_symlink(root, dest):
    """**symlink そのもの**を消す / 置き換える直前の判定 (辿った先へは書かない)。

    `os.remove(link)` はリンクを外すだけでターゲットに触らないので、root の外を
    指すリンクは「保護対象ではない」= 普通に消してよい。`is_protected` は書き込み
    経路のために root 外への解決を拒否するので、削除にはこちらを使う。
    名前規則と、root 内に解決できるときの実体規則は同じに当てる (往復 1 の B3)。
    """
    check_root_etc(root)
    guest = guest_path_of(root, dest)
    if name_is_protected(guest):
        return True
    real_root = os.path.realpath(os.path.abspath(root))
    real_dest = os.path.realpath(os.path.abspath(dest))
    if not _inside(real_root, real_dest):
        return False
    if name_is_protected(guest_path_of(real_root, real_dest)):
        return True
    st = _stat_or_none(dest)            # 辿った先 (dangling なら ENOENT)
    if st is None:
        return False
    return (st.st_dev, st.st_ino) in _protected_identities(root)


def protect_log(path, stream=None):
    """除外を明示する 1 行。失敗ではないので stdout に出す。"""
    print('protected: {} (skipped)'.format(path),
          file=stream if stream is not None else sys.stdout)


def check_dest(root, guest_path, host_src=None):
    """最終パスの確定と保護判定をまとめて行う。

    Returns: (dest_abs, protected)  — ProtectError は呼び出し側で失敗にすること。
    """
    dest = resolve_dest(root, guest_path, host_src)
    return dest, is_protected(root, dest)


if __name__ == '__main__':                                  # pragma: no cover
    if len(sys.argv) != 3:
        print('usage: deploy_protect.py <root> <guest_path>', file=sys.stderr)
        sys.exit(2)
    try:
        d, prot = check_dest(sys.argv[1], sys.argv[2])
    except ProtectError as e:
        print('ProtectError: {}'.format(e), file=sys.stderr)
        sys.exit(2)
    print('{}\t{}'.format('PROTECTED' if prot else 'writable', d))
    sys.exit(0)
