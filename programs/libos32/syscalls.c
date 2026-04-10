#include <sys/stat.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/times.h>
#include <sys/time.h>
#include <errno.h>

#undef st_atime
#undef st_mtime
#undef st_ctime

#include "os32api.h"

extern KernelAPI *kapi; /* CRT0 もしくはメインから初期化されて渡される */

#define ALIAS(f) __attribute__((weak, alias(#f)))

void __attribute__((weak)) _init(void) {}

int _read(int fd, char *ptr, int len) {
    return kapi->sys_read(fd, ptr, len);
}
int read(int fd, char *ptr, int len) ALIAS(_read);

int _write(int fd, char *ptr, int len) {
    return kapi->sys_write(fd, ptr, len);
}
int write(int fd, char *ptr, int len) ALIAS(_write);

int _open(const char *name, int flags, ...) {
    int os32_flags = 0;
    if (flags & O_WRONLY) os32_flags |= KAPI_O_WRONLY;
    else if (flags & O_RDWR) os32_flags |= KAPI_O_RDWR;
    else os32_flags |= KAPI_O_RDONLY;
    
    if (flags & O_CREAT) os32_flags |= KAPI_O_CREAT;
    if (flags & O_TRUNC) os32_flags |= KAPI_O_TRUNC;
    
    return kapi->sys_open(name, os32_flags);
}
int open(const char *name, int flags, ...) ALIAS(_open);

int _close(int fd) {
    kapi->sys_close(fd);
    return 0;
}
int close(int fd) ALIAS(_close);

int _lseek(int fd, int ptr, int dir) {
    return kapi->sys_lseek(fd, ptr, dir);
}
int lseek(int fd, int ptr, int dir) ALIAS(_lseek);

int _fstat(int fd, struct stat *st) {
    OS32_Stat os_st;
    if (kapi->sys_fstat(fd, &os_st) < 0) return -1;
    st->st_mode = os_st.st_mode;
    st->st_size = os_st.st_size;
    return 0;
}
int fstat(int fd, struct stat *st) ALIAS(_fstat);

int _stat(const char *name, struct stat *st) {
    OS32_Stat os_st;
    if (kapi->sys_stat(name, &os_st) < 0) return -1;
    st->st_mode = os_st.st_mode;
    st->st_size = os_st.st_size;
    return 0;
}
int stat_func(const char *name, struct stat *st) __attribute__((weak, alias("_stat")));

int _unlink(char *name) {
    return kapi->sys_unlink(name);
}
int unlink(char *name) ALIAS(_unlink);

int _isatty(int fd) {
    return kapi->sys_isatty(fd);
}
int isatty(int fd) ALIAS(_isatty);

void _exit(int exit_status) {
    kapi->sys_exit(exit_status);
    while (1) {}
}

int _kill(int pid, int sig) {
    errno = EINVAL;
    return -1;
}
int kill(int pid, int sig) ALIAS(_kill);

int _getpid(void) {
    return 1;
}
int getpid(void) ALIAS(_getpid);

int _gettimeofday(struct timeval *tv, void *tz) {
    if (tv) {
        tv->tv_sec = kapi->sys_time();
        tv->tv_usec = 0;
    }
    return 0;
}
int gettimeofday(struct timeval *tv, void *tz) ALIAS(_gettimeofday);

void *__env[1] = { 0 };
char **environ = (char **)__env;

int _link(char *old, char *new) {
    errno = EMLINK;
    return -1;
}
int link(char *old, char *new) ALIAS(_link);

int _execve(char *name, char **argv, char **env) {
    errno = ENOMEM;
    return -1;
}
int execve(char *name, char **argv, char **env) ALIAS(_execve);

clock_t _times(struct tms *buf) {
    return -1;
}
clock_t times(struct tms *buf) ALIAS(_times);

extern char _end[];
static char *heap_ptr = NULL;

void * _sbrk(int incr) {
    char *prev_heap_ptr;
    
    if (heap_ptr == NULL) {
        heap_ptr = (char *)&_end;
    }
    
    prev_heap_ptr = heap_ptr;
    
    /* KAPIテーブルからカーネルがセットしたヒープ上限を取得 */
    if (kapi->sbrk_heap_limit != 0 &&
        heap_ptr + incr >= (char *)kapi->sbrk_heap_limit) {
        errno = ENOMEM;
        return (void *)-1;
    }
    
    heap_ptr += incr;
    return (void *)prev_heap_ptr;
}
void * sbrk(int incr) ALIAS(_sbrk);

/*
 * ==== 最適化メモリ関数の libc 優先フック ====
 * 外部プログラム (newlib) が memcpy や memset を呼んだ際、
 * libc.a の遅い実装ではなく、カーネルのKAPI（アセンブリ実装）に強制的に向ける。
 */
void *memcpy(void *dst, const void *src, u32 n) {
    if (!kapi) return dst; /* 初期化前はスキップ (CRT0内部等で呼ばれた場合) */
    return kapi->sys_memcpy(dst, src, n);
}

void *memset(void *dst, int val, u32 n) {
    if (!kapi) {
        /* CRT0 内部等での初期化用に簡易フォールバック */
        char *d = (char *)dst;
        while (n--) *d++ = val;
        return dst;
    }
    return kapi->sys_memset(dst, val, n);
}
