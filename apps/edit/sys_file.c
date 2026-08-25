/*
 * sys_file.c - OS32 File I/O Wrapper for VZ Editor
 */
#include "vz.h"

int vz_open(const char *path, int mode) {
    if (!kapi) return -1;
    return kapi->sys_open(path, mode);
}

int vz_read(int fd, void *buf, unsigned int size) {
    if (!kapi) return -1;
    return kapi->sys_read(fd, buf, size);
}

int vz_write(int fd, const void *buf, unsigned int size) {
    if (!kapi) return -1;
    return kapi->sys_write(fd, buf, size);
}

int vz_seek(int fd, int offset, int whence) {
    if (!kapi) return -1;
    return kapi->sys_lseek(fd, offset, whence);
}

void vz_close(int fd) {
    if (!kapi) return;
    kapi->sys_close(fd);
}

unsigned int vz_get_size(int fd) {
    OS32_Stat st;
    if (!kapi) return 0;
    if (kapi->sys_fstat(fd, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

int vz_ls(const char *path, void *cb, void *ctx) {
    if (!kapi) return -1;
    return kapi->sys_ls(path, cb, ctx);
}
