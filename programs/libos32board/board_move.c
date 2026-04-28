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
