#include "skk.h"

static KernelAPI *skk_api = NULL;
static char *dict_buffer = NULL;
static int dict_size;
static char **dict_entries = NULL;
static int num_entries = 0;

static void skk_lzss_decode(const u8 *src, int src_len, u8 *dst, int dst_len) {
    int src_p = 4;
    int dst_p = 0;
    int i, j, k, r, c;
    unsigned int flags;
    u8 text_buf[4096];
    
    for (i = 0; i < 4096; i++) text_buf[i] = 0x20;
    r = 4096 - 18;
    
    while (src_p < src_len && dst_p < dst_len) {
        flags = src[src_p++];
        for (i = 0; i < 8; i++) {
            if ((flags & 1) != 0) {
                if (src_p < src_len && dst_p < dst_len) {
                    c = src[src_p++];
                    dst[dst_p++] = c;
                    text_buf[r++] = c;
                    r &= 4095;
                }
            } else {
                if (src_p + 1 < src_len) {
                    int pos, len, l;
                    j = src[src_p++];
                    k = src[src_p++];
                    pos = j | ((k & 0xF0) << 4);
                    len = (k & 0x0F) + 3;
                    for (l = 0; l < len; l++) {
                        if (dst_p < dst_len) {
                            c = text_buf[(pos + l) & 4095];
                            dst[dst_p++] = c;
                            text_buf[r++] = c;
                            r &= 4095;
                        }
                    }
                }
            }
            flags >>= 1;
            if (src_p >= src_len || dst_p >= dst_len) break;
        }
    }
}

int skk_init(const char *dict_path, KernelAPI *api) {
    int fd;
    int compressed_size;
    unsigned int original_size;
    int bytes_read;
    int i, p_idx;
    char *p;
    u8 *cmp_buf;

    skk_api = api;

    fd = skk_api->sys_open((char*)dict_path, 0);
    if (fd < 0) {
        skk_api->kprintf(0x41, "%s", "SKK: Failed to open dict\r\n");
        return -1;
    }

    {
        OS32_Stat st;
        if (skk_api->sys_fstat(fd, &st) != 0) {
            skk_api->sys_close(fd);
            return -1;
        }
        compressed_size = st.st_size;
    }

    if (compressed_size <= 4) {
        skk_api->sys_close(fd);
        return -1;
    }

    cmp_buf = (u8*)skk_api->mem_alloc(compressed_size);
    if (!cmp_buf) {
        skk_api->kprintf(0x41, "%s", "SKK: Failed to alloc cmp_buf\r\n");
        skk_api->sys_close(fd);
        return -1;
    }

    /* OS32の VFS は内部で適切に DMA/セクタを処理するため、一括で読む */
    bytes_read = skk_api->sys_read(fd, (char*)cmp_buf, compressed_size);
    skk_api->sys_close(fd);
    
    if (bytes_read != compressed_size) {
        skk_api->kprintf(0x41, "%s", "SKK: Read error\r\n");
        skk_api->mem_free(cmp_buf);
        return -1;
    }
    
    original_size = cmp_buf[0] | (cmp_buf[1] << 8) | (cmp_buf[2] << 16) | (cmp_buf[3] << 24);
    dict_size = (int)original_size;

    dict_buffer = (char*)skk_api->mem_alloc(dict_size + 1);
    if (!dict_buffer) {
        skk_api->kprintf(0x41, "%s", "SKK: Failed to alloc dict_buffer\r\n");
        skk_api->mem_free(cmp_buf);
        return -1;
    }

    /* 解凍実行 */
    skk_lzss_decode(cmp_buf, compressed_size, (u8*)dict_buffer, dict_size);
    skk_api->mem_free(cmp_buf);
    
    dict_buffer[dict_size] = '\0';

    num_entries = 0;
    p = dict_buffer;
    for (i = 0; i < dict_size; i++) {
        if (p[i] == '\n') {
            num_entries++;
        }
    }

    dict_entries = (char**)skk_api->mem_alloc((num_entries + 1) * sizeof(char*));
    if (!dict_entries) {
        skk_api->kprintf(0x41, "%s", "SKK: Failed to allocate dict_entries\r\n");
        skk_api->mem_free(dict_buffer);
        dict_buffer = NULL;
        return -1;
    }

    p_idx = 0;
    dict_entries[p_idx++] = dict_buffer;
    for (i = 0; i < dict_size; i++) {
        if (dict_buffer[i] == '\n') {
            dict_buffer[i] = '\0'; /* 改行をヌル文字にする */
            if (i + 1 < dict_size) {
                dict_entries[p_idx++] = &dict_buffer[i + 1];
            }
        } else if (dict_buffer[i] == '\r') {
            dict_buffer[i] = '\0';
        }
    }

    return 0;
}

const char *skk_dict_search(const char *midasi, char okuri) {
    int left = 0;
    int right = num_entries - 1;
    int mid;
    int cmp;
    int yomi_len;
    char search_key[256];
    char *entry;
    int i;

    if (!dict_entries || !midasi || midasi[0] == '\0') return NULL;

    yomi_len = 0;
    while (midasi[yomi_len] && yomi_len < 240) {
        search_key[yomi_len] = midasi[yomi_len];
        yomi_len++;
    }
    
    /* 送り仮名アルファベットがあればくっつける (例: "か" + 'k' -> "かk") */
    if (okuri != '\0') {
        search_key[yomi_len++] = okuri;
    }
    
    search_key[yomi_len++] = ' ';
    search_key[yomi_len] = '\0';

    /* 二分探索 */
    while (left <= right) {
        mid = left + (right - left) / 2;
        entry = dict_entries[mid];
        
        cmp = 0;
        i = 0;
        while (search_key[i] != '\0' && entry[i] != '\0') {
            if ((unsigned char)search_key[i] < (unsigned char)entry[i]) {
                cmp = -1; break;
            } else if ((unsigned char)search_key[i] > (unsigned char)entry[i]) {
                cmp = 1; break;
            }
            i++;
        }
        if (cmp == 0) {
            if (search_key[i] == '\0' && entry[i] == '\0') cmp = 0;
            /* search_key 末尾には ' ' が付与されているため、ここまで一致すれば前方一致で確定 */
            else if (search_key[i] == '\0') cmp = 0; 
            else if (entry[i] == '\0') cmp = 1;
        }

        if (cmp == 0) {
            return entry; /* 見つかった！ */
        } else if (cmp < 0) {
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }

    /* 二分探索で見つからなかった場合、リニアサーチにフォールバック */
    /* (SKK辞書の送りありセクションは降順ソートのため二分探索では見つからない) */
    for (mid = 0; mid < num_entries; mid++) {
        entry = dict_entries[mid];
        i = 0;
        cmp = 0;
        while (search_key[i] != '\0' && entry[i] != '\0') {
            if ((unsigned char)search_key[i] != (unsigned char)entry[i]) {
                cmp = 1; break;
            }
            i++;
        }
        if (cmp == 0 && search_key[i] == '\0') {
            return entry;
        }
    }

    return NULL;
}

int skk_dict_get_candidate(const char *line, int index, char *out_buf) {
    int current_idx = 0;
    int p = 0;
    int k = 0;
    
    out_buf[0] = '\0';
    if (!line) return 0;

    /* 最初の '/' を探す */
    while (line[p] && line[p] != '/') {
        p++;
    }
    if (line[p] == '\0') return 0; /* '/' がない */

    /* '/' の後から候補ブロックの走査 */
    p++;
    
    while (line[p] && line[p] != '\r' && line[p] != '\n') {
        if (current_idx == index) {
            /* 目的のインデックスに到達。次の '/' までを抽出 */
            while (line[p] && line[p] != '/' && line[p] != '\r' && line[p] != '\n') {
                out_buf[k++] = line[p++];
            }
            out_buf[k] = '\0';
            return 1; /* 成功 */
        }
        
        /* 次の '/' を探す */
        while (line[p] && line[p] != '/' && line[p] != '\r' && line[p] != '\n') {
            p++;
        }
        if (line[p] == '/') {
            p++;
            current_idx++;
        }
    }

    return 0; /* index に届かなかった */
}

void skk_free(void) {
    if (dict_entries) {
        skk_api->mem_free(dict_entries);
        dict_entries = NULL;
    }
    if (dict_buffer) {
        skk_api->mem_free(dict_buffer);
        dict_buffer = NULL;
    }
    num_entries = 0;
    dict_size = 0;
}
