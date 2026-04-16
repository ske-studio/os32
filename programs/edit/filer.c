/*
 * filer.c - OS32 VZ Editor Filer Module
 * C89 compatible
 */

#include "vz.h"

/*
 * ファイラー用ディレクトリ走査コールバック
 */
static void filer_ls_cb(const DirEntry_Ext* entry, void* ctx) {
    if (vz.filer.entry_count < MAX_FILER_ENTRIES) {
        FilerEntry* e = &vz.filer.entries[vz.filer.entry_count];
        int i;
        for (i = 0; entry->name[i] != '\0' && i < 15; i++) {
            e->name[i] = entry->name[i];
        }
        e->name[i] = '\0';
        e->size = entry->size;
        e->is_dir = (entry->type == 2);
        vz.filer.entry_count++;
    }
}

/*
 * ファイラーを開く
 */
void vz_open_filer(const char* dir) {
    int i;
    vz.filer.active = 1;
    vz.filer.entry_count = 0;
    vz.filer.cursor_idx = 0;
    vz.filer.scroll_top = 0;
    
    for (i = 0; dir[i] != '\0' && i < PATHSZ - 1; i++) {
        vz.filer.current_dir[i] = dir[i];
    }
    vz.filer.current_dir[i] = '\0';
    
    vz_ls(dir, (void*)filer_ls_cb, 0);
}

/*
 * ファイラー画面内でのキー入力を処理する
 * 戻り値: 1=画面更新が必要, 0=不要
 */
int vz_filer_handle_key(int c) {
    int kd = KEYDATA(c);
    int kc = KEYCODE(c);

    if (kd == 0x1B) { /* ESC */
        vz.filer.active = 0;
        vz_clear();
        return 1;
    } else if (kc == VK_UP || kd == 0x10) { /* Up (スキャンコード or Ctrl-P) */
        if (vz.filer.cursor_idx > 0) {
            vz.filer.cursor_idx--;
            if (vz.filer.cursor_idx < vz.filer.scroll_top) vz.filer.scroll_top = vz.filer.cursor_idx;
        }
        return 1;
    } else if (kc == VK_DOWN || kd == 0x0E) { /* Down (スキャンコード or Ctrl-N) */
        if (vz.filer.cursor_idx < vz.filer.entry_count - 1) {
            vz.filer.cursor_idx++;
            if (vz.filer.cursor_idx >= vz.filer.scroll_top + 23) vz.filer.scroll_top = vz.filer.cursor_idx - 22;
        }
        return 1;
    } else if (kd == 0x0D || kd == 0x0A) { /* Enter */
        if (vz.filer.entry_count > 0) {
            FilerEntry* e = &vz.filer.entries[vz.filer.cursor_idx];
            if (e->is_dir) {
                if (kapi) {
                    kapi->sys_chdir(e->name);
                    {
                        const char *new_cwd = kapi->sys_getcwd();
                        vz_open_filer(new_cwd ? new_cwd : "/");
                    }
                }
                return 1;
            } else {
                /* ファイルを開く処理 */
                char full_path[PATHSZ];
                int i = 0, j = 0, k = 0;
                while (vz.filer.current_dir[k] != '\0' && i < PATHSZ - 1) full_path[i++] = vz.filer.current_dir[k++];
                if (i > 0 && full_path[i - 1] != '/') full_path[i++] = '/';
                while (e->name[j] != '\0' && i < PATHSZ - 1) full_path[i++] = e->name[j++];
                full_path[i] = '\0';
                
                vz_load_file(full_path);
                
                vz.filer.active = 0;
                vz_clear();
                return 1;
            }
        }
    }
    return 0;
}
