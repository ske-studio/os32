#include "os32api.h"
#include "libos32/help.h"

KernelAPI *kapi;

extern int main(int argc, char **argv, KernelAPI *api);
extern void _init(void);

/* --help / -h / /? の簡易チェック (strcmp依存回避) */
static int _is_help_flag(const char *s)
{
    /* "--help" */
    if (s[0]=='-' && s[1]=='-' && s[2]=='h' && s[3]=='e' &&
        s[4]=='l' && s[5]=='p' && s[6]=='\0') return 1;
    /* "-h" */
    if (s[0]=='-' && s[1]=='h' && s[2]=='\0') return 1;
    /* "/?" */
    if (s[0]=='/' && s[1]=='?' && s[2]=='\0') return 1;
    return 0;
}

void _start_c(int argc, char **argv, KernelAPI *api) {
    int ret;

    kapi = api;

    _init();

    /* --help 自動ハンドリング: manページがあれば表示して終了 */
    if (argc > 1 && _is_help_flag(argv[1])) {
        if (os32_help_show(argv[0]) == 0) {
            kapi->sys_exit(0);
            while (1) {}
        }
        /* manページが見つからない場合は通常のmain()に処理を委譲 */
    }

    ret = main(argc, argv, kapi);

    kapi->sys_exit(ret);
    while (1) {}
}
