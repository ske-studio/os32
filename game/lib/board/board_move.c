/* ======================================================================== */
/*  BOARD_MOVE.C — 移動シミュレーション・先読み・最短距離                   */
/* ======================================================================== */

#include "libos32board.h"

/* 外部参照 (board_core.c で定義) */
extern BoardMass *board_get_masses(void);
extern int        board_get_mass_count(void);
extern int        board_find_index(u16 id);

/* ====================================================================== */
/*  board_walk — 直線移動シミュレーション                                   */
/* ====================================================================== */

u16 board_walk(u16 from, int steps, int *remaining)
{
    u16 cur = from;
    int left = steps;

    while (left > 0) {
        int idx = board_find_index(cur);
        const BoardMass *m;

        if (idx < 0) break;

        m = &board_get_masses()[idx];

        /* 封鎖マスに到達したら停止 */
        if (m->flags & BOARD_FLAG_BLOCKED) break;

        /* 接続なし → 行き止まり */
        if (m->connect_count == 0) break;

        /* 分岐マスに到達 → ここで停止 (ゲーム側が方向を選択する) */
        if (m->connect_count >= 2) break;

        /* 直線: connect[0] へ進む */
        cur = m->connect[0];
        if (cur == BOARD_CONNECT_NONE) break;

        left--;
    }

    if (remaining != (int *)0) {
        *remaining = left;
    }
    return cur;
}

/* ====================================================================== */
/*  board_peek_path — 指定方向のN手先マス列を取得                          */
/* ====================================================================== */

int board_peek_path(u16 from, u8 dir, u16 *out, int max)
{
    u16 cur = from;
    int count = 0;
    int idx;
    const BoardMass *m;

    if (out == (u16 *)0 || max <= 0) return 0;

    /* 最初のマスから指定方向に進む */
    idx = board_find_index(cur);
    if (idx < 0) return 0;

    m = &board_get_masses()[idx];
    if (dir >= m->connect_count) return 0;

    cur = m->connect[dir];
    if (cur == BOARD_CONNECT_NONE) return 0;

    while (count < max) {
        idx = board_find_index(cur);
        if (idx < 0) break;

        out[count] = cur;
        count++;

        m = &board_get_masses()[idx];

        /* 封鎖マス → 停止 */
        if (m->flags & BOARD_FLAG_BLOCKED) break;

        /* 接続なし → 行き止まり */
        if (m->connect_count == 0) break;

        /* 分岐マス → ここで停止 */
        if (m->connect_count >= 2) break;

        /* 次へ進む */
        cur = m->connect[0];
        if (cur == BOARD_CONNECT_NONE) break;
    }

    return count;
}

/* ====================================================================== */
/*  board_distance — BFS最短距離                                           */
/* ====================================================================== */

int board_distance(u16 from, u16 to)
{
    /* BFS用キュー (スタック上) */
    u16 queue[BOARD_BFS_QUEUE_MAX];
    i16 dist[BOARD_BFS_QUEUE_MAX];    /* 各キューエントリの距離 */
    u16 visited[BOARD_MAX_MASSES];    /* 訪問済みマスID */
    int visited_count = 0;
    int head = 0, tail = 0;
    int total = board_get_mass_count();
    int i;

    if (from == to) return 0;
    if (total == 0) return -1;

    /* 開始ノードをエンキュー */
    queue[tail] = from;
    dist[tail] = 0;
    tail++;
    visited[visited_count++] = from;

    while (head < tail) {
        u16 cur = queue[head];
        i16 cur_dist = dist[head];
        int idx;
        const BoardMass *m;

        head++;

        idx = board_find_index(cur);
        if (idx < 0) continue;

        m = &board_get_masses()[idx];

        /* 封鎖マスはスキップ (開始マスを除く) */
        if (cur != from && (m->flags & BOARD_FLAG_BLOCKED)) continue;

        /* 隣接ノードを探索 */
        for (i = 0; i < (int)m->connect_count; i++) {
            u16 next = m->connect[i];
            int already = 0;
            int v;

            if (next == BOARD_CONNECT_NONE) continue;

            /* 目的地に到達 */
            if (next == to) return (int)(cur_dist + 1);

            /* 訪問済みチェック */
            for (v = 0; v < visited_count; v++) {
                if (visited[v] == next) {
                    already = 1;
                    break;
                }
            }
            if (already) continue;

            /* キューとvisited配列のオーバーフロー防止 */
            if (tail >= BOARD_BFS_QUEUE_MAX) continue;
            if (visited_count >= BOARD_MAX_MASSES) continue;

            queue[tail] = next;
            dist[tail] = cur_dist + 1;
            tail++;
            visited[visited_count++] = next;
        }
    }

    return -1;  /* 到達不能 */
}

/* ====================================================================== */
/*  Dijkstra法 内部実装                                                     */
/*  cost=0 のマスは通行コスト1として扱う (全マスが最低コスト1)              */
/* ====================================================================== */

/* ノードのコスト情報 (Dijkstra用) */
typedef struct {
    u16  id;
    i16  cost;         /* 始点からの累積コスト (-1=未到達) */
    u16  prev;         /* 最短経路上の前ノード (0xFFFF=なし) */
    u8   visited;
    u8   _pad;
} DijkNode;

/* 未訪問ノードの中から最小コストのインデックスを返す (-1=全訪問済み) */
static int dijk_find_min(DijkNode *nodes, int count)
{
    int best = -1;
    i16 best_cost = 0x7FFF;
    int i;

    for (i = 0; i < count; i++) {
        if (!nodes[i].visited && nodes[i].cost >= 0 &&
            nodes[i].cost < best_cost) {
            best_cost = nodes[i].cost;
            best = i;
        }
    }
    return best;
}

/* Dijkstra法のコア: ノード配列を構築し全最短経路を計算する
 * 戻り値: to_id に対応するノードインデックス (-1=未発見)
 */
static int dijk_solve(u16 from, u16 to, DijkNode *nodes)
{
    BoardMass *masses = board_get_masses();
    int total = board_get_mass_count();
    int from_idx = -1;
    int to_idx = -1;
    int i;

    /* ノード配列を初期化 */
    for (i = 0; i < total; i++) {
        nodes[i].id = masses[i].id;
        nodes[i].cost = -1;       /* 未到達 */
        nodes[i].prev = 0xFFFF;
        nodes[i].visited = 0;

        if (masses[i].id == from) from_idx = i;
        if (masses[i].id == to)   to_idx = i;
    }

    if (from_idx < 0 || to_idx < 0) return -1;

    /* 始点のコストを0に設定 */
    nodes[from_idx].cost = 0;

    for (;;) {
        int u = dijk_find_min(nodes, total);
        int ui;
        const BoardMass *m;

        if (u < 0) break;  /* 全ノード訪問済み or 到達可能ノードなし */

        /* 目的地に到達 → 早期終了 */
        if (nodes[u].id == to) break;

        nodes[u].visited = 1;
        ui = board_find_index(nodes[u].id);
        if (ui < 0) continue;

        m = &masses[ui];

        /* 封鎖マスはスキップ (始点を除く) */
        if (nodes[u].id != from && (m->flags & BOARD_FLAG_BLOCKED)) continue;

        /* 隣接ノードを走査 */
        for (i = 0; i < (int)m->connect_count; i++) {
            u16 next_id = m->connect[i];
            int ni;
            int next_ni;
            i16 edge_cost;
            i16 new_cost;

            if (next_id == BOARD_CONNECT_NONE) continue;

            /* 隣接マスのノードインデックスを検索 */
            next_ni = -1;
            for (ni = 0; ni < total; ni++) {
                if (nodes[ni].id == next_id) {
                    next_ni = ni;
                    break;
                }
            }
            if (next_ni < 0) continue;
            if (nodes[next_ni].visited) continue;

            /* エッジコスト = 移動先マスのcost (0の場合は1) */
            {
                int mi = board_find_index(next_id);
                if (mi < 0) continue;
                edge_cost = (i16)masses[mi].cost;
                if (edge_cost <= 0) edge_cost = 1;
            }

            new_cost = nodes[u].cost + edge_cost;

            if (nodes[next_ni].cost < 0 || new_cost < nodes[next_ni].cost) {
                nodes[next_ni].cost = new_cost;
                nodes[next_ni].prev = nodes[u].id;
            }
        }
    }

    return to_idx;
}

/* ====================================================================== */
/*  board_distance_cost — コスト付き最短距離                                */
/* ====================================================================== */

int board_distance_cost(u16 from, u16 to)
{
    DijkNode nodes[BOARD_MAX_MASSES];
    int to_idx;

    if (from == to) return 0;
    if (board_get_mass_count() == 0) return -1;

    to_idx = dijk_solve(from, to, nodes);
    if (to_idx < 0) return -1;

    return (int)nodes[to_idx].cost;
}

/* ====================================================================== */
/*  board_find_path — コスト付き最短経路取得                                */
/* ====================================================================== */

int board_find_path(u16 from, u16 to, u16 *out, int max)
{
    DijkNode nodes[BOARD_MAX_MASSES];
    int to_idx;
    int total;
    u16 trace[BOARD_MAX_MASSES];
    int trace_count = 0;
    u16 cur;
    int i;

    if (out == (u16 *)0 || max <= 0) return -1;
    if (from == to) return 0;

    total = board_get_mass_count();
    if (total == 0) return -1;

    to_idx = dijk_solve(from, to, nodes);
    if (to_idx < 0 || nodes[to_idx].cost < 0) return -1;

    /* 経路を逆順にたどる (to → from) */
    cur = to;
    while (cur != from && trace_count < BOARD_MAX_MASSES) {
        int ni;
        int found = 0;

        trace[trace_count++] = cur;

        /* cur の prev を検索 */
        for (ni = 0; ni < total; ni++) {
            if (nodes[ni].id == cur) {
                cur = nodes[ni].prev;
                found = 1;
                break;
            }
        }
        if (!found || cur == 0xFFFF) break;
    }

    /* from自身に到達できなかった場合 */
    if (cur != from) return -1;

    /* 逆順を反転して out に格納 */
    {
        int out_count = trace_count;
        if (out_count > max) out_count = max;

        for (i = 0; i < out_count; i++) {
            out[i] = trace[trace_count - 1 - i];
        }
        return out_count;
    }
}
