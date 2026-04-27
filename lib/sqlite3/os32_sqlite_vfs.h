/* ======================================================================== */
/*  OS32_SQLITE_VFS.H — OS32 カスタム SQLite VFS ヘッダ                     */
/* ======================================================================== */
#ifndef OS32_SQLITE_VFS_H
#define OS32_SQLITE_VFS_H

/* SQLite エンジン初期化 (MEMSYS5 + VFS登録 + sqlite3_initialize) */
/* 戻り値: 0=成功, 非0=失敗 */
int os32_sqlite_init(void);

/* カーネル内 SQLite 動作テスト (メモリDB で CRUD 検証) */
/* 戻り値: 0=全テスト成功, -1=失敗あり */
int os32_sqlite_test(void);

/* MEMSYS5 プール canary 検証 (デバッグ用) */
/* 戻り値: 0=正常, -1=前方破壊, -2=後方破壊 */
int memsys5_check_canary(void);

/* VFS メソッドテーブルアドレスダンプ (デバッグ用) */
void os32_sqlite_dump_vfs(void);

#endif /* OS32_SQLITE_VFS_H */
