/* ======================================================================== */
/*  CFG_ENUM.C — cfg_enum / cfg_enum_scopes (票 §1-5)                       */
/*                                                                          */
/*  行は全部 private 配列へ写してから callback を呼ぶ。SHM の row は次の DB   */
/*  操作で無効になるので、callback の中で値を取らせない (key を集めてから外  */
/*  で get する) 設計にしている。列挙の作業領域 (17KB) をここに閉じ込めて、  */
/*  cfg_get だけのアプリが背負わないよう独立した翻訳単位にしてある。         */
/* ======================================================================== */

#include "cfg_internal.h"

/* prefix は LIKE を使わない。`_` / `%` を含む key でも前方一致にするため
 * substr(key, 1, <len>) = <prefix> で比べる (票 §1-5)。 */
static const char SQL_ENUM[] =
    "SELECT key, type FROM settings"
    " WHERE scope=? AND substr(key,1,?)=? ORDER BY key";
static const char SQL_SCOPES[] =
    "SELECT DISTINCT scope FROM settings ORDER BY scope";

static char e_keys[CFG_ENUM_MAX][CFG_KEY_MAX + 1];
static int  e_types[CFG_ENUM_MAX];
static char e_scopes[CFG_SCOPES_MAX][CFG_SCOPE_MAX + 1];

static int copy_name(char *dst, int cap, const char *src, int len)
{
    int i;
    if (!src || len < 0 || len >= cap) return -1;
    for (i = 0; i < len; i++) dst[i] = src[i];
    dst[len] = '\0';
    return 0;
}

static int enum_ready(CfgDb *db)
{
    if (!db || !db->in_use) return 0;
    if (db->in_enum) return 0;
    if (db->handle < 0) return 0;
    return db->status == CFG_OK || db->status == CFG_VERSION;
}

int cfg_enum(CfgDb *db, const char *scope, const char *prefix,
             int (*fn)(const char *key, int type, void *ctx), void *ctx)
{
    const CfgBackend *b = cfg_backend();
    int n = 0, i, rc, plen, slen;
    static const char empty[1] = { 0 };

    if (!fn) return OS32_ERR_INVAL;
    if (!cfg_i_len_ok(scope, CFG_SCOPE_MAX)) return OS32_ERR_INVAL;
    if (!db || !db->in_use) return OS32_ERR_INVAL;
    if (db->in_enum) return OS32_ERR_INVAL;             /* 再入は拒否 */
    if (!enum_ready(db)) return 0;                      /* MISSING / CORRUPT */

    plen = 0;
    if (prefix) {
        while (prefix[plen]) {
            if (plen >= CFG_KEY_MAX) return OS32_ERR_INVAL;
            plen++;
        }
    }
    slen = 0;
    while (scope[slen]) slen++;

    if (cfg_i_prepare(db, SQL_ENUM) != 0) {
        db->status = CFG_ERROR;
        return OS32_ERR_IO;
    }
    if (b->db_bind_text(db->handle, 1, scope, slen) != 0 ||
        b->db_bind_int(db->handle, 2, plen) != 0 ||
        b->db_bind_text(db->handle, 3, plen ? prefix : empty, plen) != 0) {
        cfg_i_note(db);
        b->db_finalize(db->handle);
        return OS32_ERR_IO;
    }
    for (;;) {
        rc = b->db_step(db->handle);
        if (rc == DB_STATUS_DONE) break;
        if (rc != DB_STATUS_ROW) {
            cfg_i_note(db);
            b->db_finalize(db->handle);
            db->status = CFG_ERROR;
            return OS32_ERR_IO;
        }
        if (n >= CFG_ENUM_MAX) {            /* 257 件目 — callback は呼ばない */
            b->db_finalize(db->handle);
            return OS32_ERR_NOSPC;
        }
        if (cfg_i_col_type(0) != DB_TYPE_TEXT ||
            copy_name(e_keys[n], (int)sizeof(e_keys[0]),
                      (const char *)cfg_i_col_ptr(0), cfg_i_col_len(0)) != 0) {
            b->db_finalize(db->handle);
            db->status = CFG_ERROR;
            return OS32_ERR_IO;
        }
        e_types[n] = (cfg_i_col_type(1) == DB_TYPE_INT)
                     ? (int)cfg_i_col_int(1) : -1;
        n++;
    }
    b->db_finalize(db->handle);

    db->in_enum = 1;
    for (i = 0; i < n; i++) {
        if (fn(e_keys[i], e_types[i], ctx) != 0) { i++; break; }
    }
    db->in_enum = 0;
    return i;
}

int cfg_enum_scopes(CfgDb *db, int (*fn)(const char *scope, void *ctx),
                    void *ctx)
{
    const CfgBackend *b = cfg_backend();
    int n = 0, i, rc;

    if (!fn) return OS32_ERR_INVAL;
    if (!db || !db->in_use) return OS32_ERR_INVAL;
    if (db->in_enum) return OS32_ERR_INVAL;
    if (!enum_ready(db)) return 0;

    if (cfg_i_prepare(db, SQL_SCOPES) != 0) {
        db->status = CFG_ERROR;
        return OS32_ERR_IO;
    }
    for (;;) {
        rc = b->db_step(db->handle);
        if (rc == DB_STATUS_DONE) break;
        if (rc != DB_STATUS_ROW) {
            cfg_i_note(db);
            b->db_finalize(db->handle);
            db->status = CFG_ERROR;
            return OS32_ERR_IO;
        }
        if (n >= CFG_SCOPES_MAX) {
            b->db_finalize(db->handle);
            return OS32_ERR_NOSPC;
        }
        if (cfg_i_col_type(0) != DB_TYPE_TEXT ||
            copy_name(e_scopes[n], (int)sizeof(e_scopes[0]),
                      (const char *)cfg_i_col_ptr(0), cfg_i_col_len(0)) != 0) {
            b->db_finalize(db->handle);
            db->status = CFG_ERROR;
            return OS32_ERR_IO;
        }
        n++;
    }
    b->db_finalize(db->handle);

    db->in_enum = 1;
    for (i = 0; i < n; i++) {
        if (fn(e_scopes[i], ctx) != 0) { i++; break; }
    }
    db->in_enum = 0;
    return i;
}
