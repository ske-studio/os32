#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_worldmap.py — 2Dワールドマップ版の盤面データ生成 (試作)

現行の gen_board_db.py が作る「行ごとに独立した1本道 x 8」を、
上下左右が実際に繋がる 2D グリッドへ作り直すための試作。

参考にした構造 (ドカポン3・2・1 ~嵐を呼ぶ友情~ / SFC):
  - 世界地図をベースにした 2D フィールド。マスは地形の上に置かれる
  - 大陸ごとに章が進む (全8章)。大陸あたりの村は7つ程度
  - 大陸間は森・洞窟・ワープで接続 (直接地続きではない)
  - 一方通行マスがある
  - 地形そのものに効果があるマスがある (毒の沼=通過で毒 / 雪原=止まるとマヒ)
  - 村はモンスターが支配していて、倒すと統治できる
  - 集金所で「統治村から1つ選んで集金 or 投資」を行う
  - 店は曜日で挙動が変わる (土曜25%引き / 日曜定休)
  - 宝箱は色で中身が違う (青=道具 / 黄=魔法 / 赤=罠込み)
名称・配置は本作固有のもの(記紀由来)で、構造だけを参考にしている。

■ 形の作り方
手書き ASCII は「繋がっているつもりで繋がっていない」事故が起きるので、
リング(外周ループ)を生成してから施設文字を並べる方式にした。
リングは必ず一本に繋がることが保証される。

  リング: 幅9 x 高7 の外周 = 24マス
  ゲート: 左右に1マスずつ張り出す = +2
  枝    : 下辺中央から内側へ 2マス = +2   (分岐と行き止まりを作る)
  計 28マス/大陸。 8大陸 = 224、オノコロ島 24 で合計 248 (上限256)

■ 施設文字
  #平地 F森 M岩場 D砂漠 s毒沼 w雪原
  V村  !モンスター  I道具屋 E武器防具屋 A魔法屋 C教会
  K城  $集金所  B青宝箱 Y黄宝箱 R赤宝箱  O魔法陣  G ゲート  ^ダンジョン
"""

import argparse
import os
import sys
from collections import Counter

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJ_DIR = os.path.dirname(TOOLS_DIR)

BOARD_MAX_MASSES = 384
BOARD_MAX_AREAS = 16
BOARD_MAX_CONNECT = 8

MASS_EMPTY, MASS_VILLAGE, MASS_BATTLE, MASS_TREASURE = 0, 1, 2, 3
MASS_EQUIP_SHOP, MASS_ITEM_SHOP, MASS_MAGIC_SHOP = 4, 5, 6
MASS_CHURCH, MASS_CIRCLE, MASS_EVENT = 7, 8, 9
MASS_GATE, MASS_CASTLE, MASS_MAGIC_CHEST = 10, 11, 12
MASS_COLLECT = 13   # 集金所 (新設)
MASS_DUNGEON = 14   # ダンジョン入口 (新設)

TERR_PLAIN, TERR_FOREST, TERR_ROCK, TERR_DESERT = 0, 1, 2, 3
TERR_SWAMP, TERR_SNOW = 4, 5

CHARS = {
    '#': (MASS_EMPTY,       TERR_PLAIN),
    'F': (MASS_EMPTY,       TERR_FOREST),
    'M': (MASS_EMPTY,       TERR_ROCK),
    'D': (MASS_EMPTY,       TERR_DESERT),
    's': (MASS_EMPTY,       TERR_SWAMP),
    'w': (MASS_EMPTY,       TERR_SNOW),
    'V': (MASS_VILLAGE,     TERR_PLAIN),
    '!': (MASS_BATTLE,      TERR_PLAIN),
    'I': (MASS_ITEM_SHOP,   TERR_PLAIN),
    'E': (MASS_EQUIP_SHOP,  TERR_PLAIN),
    'A': (MASS_MAGIC_SHOP,  TERR_PLAIN),
    'C': (MASS_CHURCH,      TERR_PLAIN),
    'K': (MASS_CASTLE,      TERR_PLAIN),
    '$': (MASS_COLLECT,     TERR_PLAIN),
    'B': (MASS_TREASURE,    TERR_PLAIN),
    'Y': (MASS_MAGIC_CHEST, TERR_PLAIN),
    'R': (MASS_TREASURE,    TERR_ROCK),
    'O': (MASS_CIRCLE,      TERR_PLAIN),
    'G': (MASS_GATE,        TERR_PLAIN),
    '^': (MASS_DUNGEON,     TERR_ROCK),
}

TYPE_NAME = {
    MASS_EMPTY: 'Plain', MASS_VILLAGE: 'Village', MASS_BATTLE: 'Monster',
    MASS_TREASURE: 'Chest', MASS_EQUIP_SHOP: 'Weapon', MASS_ITEM_SHOP: 'Item',
    MASS_MAGIC_SHOP: 'Magic', MASS_CHURCH: 'Shrine', MASS_CIRCLE: 'Circle',
    MASS_EVENT: 'Event', MASS_GATE: 'Gate', MASS_CASTLE: 'Castle',
    MASS_MAGIC_CHEST: 'GoldChest', MASS_COLLECT: 'Collect',
    MASS_DUNGEON: 'Dungeon',
}
TERR_NAME = {TERR_PLAIN: 'plain', TERR_FOREST: 'forest', TERR_ROCK: 'rock',
             TERR_DESERT: 'desert', TERR_SWAMP: 'swamp', TERR_SNOW: 'snow'}

REGIONS = []


def region(area, name, origin, art, unlock_by=None):
    """ASCII でリージョンの形を定義する。'.' は非マス(海)。

    上下左右に隣接するマスどうしが自動で接続される。
    繋がり漏れは check() が検出するので、形は素直に描いてよい。
    """
    lines = [ln.rstrip() for ln in art.strip('\n').split('\n')]
    REGIONS.append(dict(area=area, name=name, origin=origin,
                        art=lines, unlock_by=unlock_by))


# ---------------------------------------------------------------------------
#  リージョン定義 — 日本列島
#
#  九州(0) を起点に、四国 -> 中国 -> 近畿 -> 中部 -> 関東 -> 東北 -> 北海道 と
#  北上して開放される。沖縄は離島として中盤に開放。
#  origin は世界グリッド上の左上。列島の並びに合わせて南西->北東へ配置する。
# ---------------------------------------------------------------------------

# ■ 形を描くときの決まり
#   輪郭が曲がるところは必ず「階段」を入れること。
#   斜めは接続にならないので、角を素直に落とすと道が千切れる。
#     悪い例   ..##..     良い例   ..##..
#              .#..#.             .##..##.

# 九州: 南北に長い楕円 + 南へ伸びる尾 (薩摩)
region(0, 'Kyushu', (6, 40), """
...V#K#...
..##..##..
..V....V..
..I....#..
..V#$..V..
..#....#..
..V....V..
.G##..##..
..V#^#V...
...V......
...V......
""")

# 四国: 横長の楕円
region(1, 'Shikoku', (18, 51), """
..V#F#V#..
.G#....#V.
.!......A.
.##....##.
..V#C#V#..
""", unlock_by=0)

# 中国: 東西に長い。山陰と山陽の二本道
region(2, 'Chugoku', (15, 42), """
..V#F#V#B#V#..
.G#........#V.
.V..........#.
.##........##.
..V#C#V#E#^#..
""", unlock_by=1)

# 近畿: 本体は丸く、南へ紀伊半島
region(3, 'Kinki', (29, 39), """
..V#F#V#..
.G#$...#V.
.I......!.
.##....##.
..V#C#V#..
....#.....
....V.....
""", unlock_by=2)

# 中部: 大きめ。北へ能登半島
region(4, 'Chubu', (36, 30), """
....V.....
....#.....
..V#^#V#..
.G#.$..#V.
.E......!.
.#......V.
.##....##.
..V#B#V#..
""", unlock_by=3)

# 関東: 丸い平野
region(5, 'Kanto', (45, 35), """
..V#V#..
.##$.##.
.M....V.
.G....A.
.##..##.
..V#B#..
""", unlock_by=4)

# 東北: 南北に長い
region(6, 'Tohoku', (48, 23), """
..V#V#..
.##$.##.
.w....V.
.#....#.
.G....w.
.#....V.
.C....I.
.#....V.
.##..##.
..V#B#..
""", unlock_by=5)

# 北海道: 菱形
region(7, 'Hokkaido', (53, 13), """
...V#V...
..w#.#w..
.V#...#V.
G#.....!.
.$.....O.
.V#...#V.
..C#.#^..
...V#V...
""", unlock_by=6)

# 沖縄: 小さな環状の島
region(8, 'Okinawa', (2, 57), """
.V#V#.
.#..#.
.G..C.
.V..#.
.V#V#.
""", unlock_by=2)


# 一方通行: (区画, (col1,row1), (col2,row2)) — 1 から 2 へしか進めない
#
# 迂回路がある区間にだけ置くこと。行き止まりや唯一の経路に置くと
# プレイヤーが詰む。check() が「方向を考慮した到達性」で検出する。
ONEWAY_EDGES = [
    # 中国: 山陰(北)の道を東向きの一方通行に。戻りは山陽(南)の道を使う
    (2, (4, 0), (5, 0)),
    (2, (5, 0), (6, 0)),
    # 東北: 奥羽の西側を北向きの一方通行に。東側の道で戻れる
    (6, (1, 5), (1, 4)),
]


# ゲート接続: (区画A, A内のG番号, 区画B, B内のG番号)
# G は ASCII 内での出現順 (上->下, 左->右) に 0 から番号がつく
GATE_LINKS = [
    (0, 0, 1, 0),   # 九州 <-> 四国
    (1, 0, 2, 0),   # 四国 <-> 中国
    (2, 0, 3, 0),   # 中国 <-> 近畿
    (3, 0, 4, 0),   # 近畿 <-> 中部
    (4, 0, 5, 0),   # 中部 <-> 関東
    (5, 0, 6, 0),   # 関東 <-> 東北
    (6, 0, 7, 0),   # 東北 <-> 北海道
    (0, 0, 8, 0),   # 九州 <-> 沖縄
]


def build():
    cells = {}
    gate_index = {}     # (area, n番目) -> (wx, wy)

    for reg in REGIONS:
        ox, oy = reg['origin']
        gn = 0
        for ry, line in enumerate(reg['art']):
            for rx, ch in enumerate(line):
                if ch in ('.', ' '):
                    continue
                if ch not in CHARS:
                    raise ValueError('未知の文字 %r (%s row %d col %d)'
                                     % (ch, reg['name'], ry, rx))
                mtype, terr = CHARS[ch]
                wp = (ox + rx, oy + ry)
                if wp in cells:
                    raise ValueError('マスが重複: %s と %s が %s で衝突'
                                     % (reg['name'], cells[wp]['region'], wp))
                cells[wp] = dict(type=mtype, terrain=terr, area=reg['area'],
                                 region=reg['name'], ch=ch)
                if ch == 'G':
                    gate_index[(reg['area'], gn)] = wp
                    gn += 1

    order = sorted(cells.keys(), key=lambda p: (cells[p]['area'], p[1], p[0]))
    ids = {p: i for i, p in enumerate(order)}
    masses = [dict(id=ids[p], x=p[0], y=p[1], **cells[p]) for p in order]

    # 一方通行の辺を世界座標の集合にしておく
    oneway = set()
    for (area, c1, c2) in ONEWAY_EDGES:
        reg = next((r for r in REGIONS if r['area'] == area), None)
        if reg is None:
            raise ValueError('ONEWAY_EDGES が未知の区画 %d を参照' % area)
        ox, oy = reg['origin']
        p1 = (ox + c1[0], oy + c1[1])
        p2 = (ox + c2[0], oy + c2[1])
        if p1 not in cells or p2 not in cells:
            raise ValueError('ONEWAY_EDGES の端点がマスでない: %s -> %s'
                             % (p1, p2))
        oneway.add((p1, p2))

    conns = []
    for p in order:
        x, y = p
        for (dx, dy) in ((1, 0), (0, 1)):
            q = (x + dx, y + dy)
            if q not in cells:
                continue
            if (p, q) in oneway:
                conns.append((ids[p], ids[q], 0))    # p -> q のみ
            elif (q, p) in oneway:
                conns.append((ids[q], ids[p], 0))    # q -> p のみ
            else:
                conns.append((ids[p], ids[q], 1))

    # ゲート同士 (離れた座標どうしを論理接続する)
    gate_conns = []
    for (a1, n1, a2, n2) in GATE_LINKS:
        p1, p2 = gate_index.get((a1, n1)), gate_index.get((a2, n2))
        if p1 is None or p2 is None:
            raise ValueError('GATE_LINKS が参照するゲートがない: '
                             '(area=%s,#%s) <-> (area=%s,#%s)'
                             % (a1, n1, a2, n2))
        gate_conns.append((ids[p1], ids[p2], 1))

    return masses, conns, gate_conns, ids, cells


def check(masses, conns, gate_conns):
    problems, info = [], []
    n = len(masses)
    info.append('マス数        : %d / %d' % (n, BOARD_MAX_MASSES))
    if n > BOARD_MAX_MASSES:
        problems.append('マス数が上限 %d を超過' % BOARD_MAX_MASSES)

    areas = sorted(set(m['area'] for m in masses))
    info.append('区画数        : %d / %d' % (len(areas), BOARD_MAX_AREAS))
    if len(areas) > BOARD_MAX_AREAS:
        problems.append('区画数が上限超過')

    tc = Counter(m['type'] for m in masses)
    info.append('村            : %d' % tc[MASS_VILLAGE])
    info.append('種別内訳      : ' + ', '.join(
        '%s=%d' % (TYPE_NAME.get(t, t), c) for t, c in sorted(tc.items())))
    tr = Counter(m['terrain'] for m in masses)
    info.append('地形内訳      : ' + ', '.join(
        '%s=%d' % (TERR_NAME.get(t, t), c) for t, c in sorted(tr.items())))

    alle = conns + gate_conns
    deg = Counter()
    for (a, b, _) in alle:
        deg[a] += 1
        deg[b] += 1
    for m in masses:
        if deg[m['id']] > BOARD_MAX_CONNECT:
            problems.append('マス %d の接続数 %d が上限超過'
                            % (m['id'], deg[m['id']]))
    dh = Counter(deg[m['id']] for m in masses)
    info.append('接続数の分布  : ' + ', '.join(
        '%d本=%d' % (k, v) for k, v in sorted(dh.items())))
    branch = sum(v for k, v in dh.items() if k >= 3)
    ratio = branch * 100 // max(1, n)
    info.append('分岐マス(3本+): %d (%d%%)' % (branch, ratio))
    if ratio > 20:
        problems.append('分岐が多すぎる (%d%%)。道が塗り潰しになっていないか。'
                        ' ほぼ毎歩で進路選択が発生して遊べない' % ratio)
    dead = sum(v for k, v in dh.items() if k == 1)
    info.append('行き止まり    : %d' % dead)

    iso = [m for m in masses if deg[m['id']] == 0]
    if iso:
        for m in iso[:8]:
            reg = next((r for r in REGIONS if r['name'] == m['region']), None)
            lx = m['x'] - reg['origin'][0] if reg else '?'
            ly = m['y'] - reg['origin'][1] if reg else '?'
            problems.append("孤立マス #%d '%s' — %s の row %s col %s "
                            "(斜めにしか隣がない)"
                            % (m['id'], m['ch'], m['region'], ly, lx))

    # 区画ごとの連結性 (どこが千切れているかを局所座標で報告する)
    adj_local = {}
    for (a, b, _) in conns:
        adj_local.setdefault(a, []).append(b)
        adj_local.setdefault(b, []).append(a)
    by_area = {}
    for m in masses:
        by_area.setdefault(m['area'], []).append(m)
    for area, mem in sorted(by_area.items()):
        ids_in = set(x['id'] for x in mem)
        seen_a, stack_a = {mem[0]['id']}, [mem[0]['id']]
        while stack_a:
            cur = stack_a.pop()
            for nb in adj_local.get(cur, []):
                if nb in ids_in and nb not in seen_a:
                    seen_a.add(nb)
                    stack_a.append(nb)
        if len(seen_a) != len(mem):
            reg = next((r for r in REGIONS if r['area'] == area), None)
            lost = [x for x in mem if x['id'] not in seen_a]
            where = ', '.join(
                'row %d col %d' % (x['y'] - reg['origin'][1],
                                   x['x'] - reg['origin'][0])
                for x in lost[:6]) if reg else ''
            problems.append('区画 %d (%s) が分断: %d/%d 到達。切れている側: %s'
                            % (area, mem[0]['region'], len(seen_a), len(mem),
                               where))

    # 全体が1つに繋がっているか (ゲート込み)
    adj = {}
    for (a, b, _) in alle:
        adj.setdefault(a, []).append(b)
        adj.setdefault(b, []).append(a)
    seen, stack = {0}, [0]
    while stack:
        cur = stack.pop()
        for nb in adj.get(cur, []):
            if nb not in seen:
                seen.add(nb)
                stack.append(nb)
    if len(seen) != n:
        problems.append('マップ全体が繋がっていない (%d/%d 到達)'
                        % (len(seen), n))
    else:
        info.append('連結性        : 全 %d マスに到達可能' % n)

    # 一方通行を入れると片道になって詰む可能性があるので、
    # 「どのマスからどのマスへも行けるか」を有向グラフで確かめる
    dirs = {}
    for (a, b, bd) in alle:
        dirs.setdefault(a, set()).add(b)
        if bd:
            dirs.setdefault(b, set()).add(a)

    def reach(src):
        seen_d, st = {src}, [src]
        while st:
            cur = st.pop()
            for nb in dirs.get(cur, ()):
                if nb not in seen_d:
                    seen_d.add(nb)
                    st.append(nb)
        return seen_d

    ow = [e for e in alle if e[2] == 0]
    info.append('一方通行      : %d 本' % len(ow))
    if ow:
        # 一方通行の出口側から全体に戻れるか (代表点で確認)
        start_id = masses[0]['id']
        fwd = reach(start_id)
        if len(fwd) != n:
            problems.append('一方通行のせいで到達できないマスがある (%d/%d)'
                            % (len(fwd), n))
        else:
            # 逆に、各一方通行の先から出発点へ戻れるか
            for (a, b, _) in ow:
                if start_id not in reach(b):
                    problems.append('一方通行 %d->%d の先から戻れない (詰む)'
                                    % (a, b))
                    break
            else:
                info.append('一方通行の安全性: 迂回路あり (どこからでも戻れる)')

    info.append('ゲート接続    : %d 本' % len(gate_conns))
    return info, problems


def assign_village_ids(masses):
    """村マスに econ.db estates.id (1..59) を id 昇順で割り当て、param に入れる。
    村以外の param は 0。"""
    vid = 0
    for m in masses:
        if m['type'] == MASS_VILLAGE:
            vid += 1
            m['param'] = vid
        else:
            m['param'] = 0
    return vid


SCHEMA = """
CREATE TABLE masses (
    id      INTEGER PRIMARY KEY,
    type    INTEGER NOT NULL,
    area    INTEGER NOT NULL DEFAULT 0,
    param   INTEGER DEFAULT 0,
    cost    INTEGER DEFAULT 1,
    flags   INTEGER DEFAULT 0,
    x       INTEGER DEFAULT 0,
    y       INTEGER DEFAULT 0,
    terrain INTEGER DEFAULT 0
);

CREATE TABLE connections (
    from_id       INTEGER NOT NULL,
    to_id         INTEGER NOT NULL,
    bidirectional INTEGER NOT NULL DEFAULT 1,
    PRIMARY KEY (from_id, to_id)
);

CREATE TABLE areas (
    id           INTEGER PRIMARY KEY,
    unlock_type  INTEGER NOT NULL DEFAULT 0,
    unlock_param INTEGER DEFAULT 0
);
"""


def write_db(masses, conns, gate_conns, path):
    import sqlite3

    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        os.remove(path)

    con = sqlite3.connect(path)
    c = con.cursor()
    c.executescript(SCHEMA)

    c.executemany(
        'INSERT INTO masses (id,type,area,param,cost,flags,x,y,terrain)'
        ' VALUES (?,?,?,?,?,?,?,?,?)',
        [(m['id'], m['type'], m['area'], m['param'], 1, 0,
          m['x'], m['y'], m['terrain']) for m in masses])

    # 重複除去。ここで (a,b) をソートしてはいけない — 一方通行の向きが消える。
    # 双方向の辺は 1 行だけ入れれば board_core が両向きに展開する。
    seen = set()
    rows = []
    for (a, b, bd) in conns + gate_conns:
        if (a, b) in seen:
            continue
        seen.add((a, b))
        rows.append((a, b, bd))
    c.executemany('INSERT INTO connections VALUES (?,?,?)', rows)

    # 区画: unlock_type 0=初期解放, 1=ボス撃破(unlock_param=前区画)
    areas = []
    for reg in REGIONS:
        if reg['unlock_by'] is None:
            areas.append((reg['area'], 0, 0))
        else:
            areas.append((reg['area'], 1, reg['unlock_by']))
    c.executemany('INSERT INTO areas VALUES (?,?,?)', sorted(areas))

    con.commit()
    con.close()
    return len(rows), len(areas)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--png', action='store_true')
    ap.add_argument('--write', action='store_true',
                    help='assets/board.db を生成する')
    args = ap.parse_args()

    masses, conns, gate_conns, ids, cells = build()
    nvil = assign_village_ids(masses)
    info, problems = check(masses, conns, gate_conns)
    info.append('村ID (param)  : 1..%d (econ.db estates.id と対応)' % nvil)
    if nvil != 59:
        problems.append('村が %d 件。econ.db の estates は 59 件なので一致させること'
                        % nvil)

    print('=== ワールドマップ (試作) ===')
    for line in info:
        print('  ' + line)
    if problems:
        print('--- 問題 ---')
        for p in problems:
            print('  ! ' + p)
    else:
        print('  問題なし')

    if args.png:
        sys.path.insert(0, TOOLS_DIR)
        from uimock_world import render_overview
        out = os.path.join(PROJ_DIR, 'docs', 'tasks', 'game', 'ui',
                           'worldmap.png')
        render_overview(masses, conns, gate_conns, out)
        print('  俯瞰図 -> %s' % os.path.normpath(out))

    if args.write:
        if problems:
            print('  ! 問題があるので board.db は書き出さない')
            return 1
        out = os.path.join(PROJ_DIR, 'assets', 'board.db')
        nc, na = write_db(masses, conns, gate_conns, out)
        print('  board.db -> %s' % os.path.normpath(out))
        print('    %d masses, %d connections, %d areas' % (len(masses), nc, na))

    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
