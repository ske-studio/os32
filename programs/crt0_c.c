#include "os32api.h"

KernelAPI *kapi;

extern int main(int argc, char **argv, KernelAPI *api);
extern void _init(void);

void _start_c(int argc, char **argv, KernelAPI *api) {
    kapi = api;
    
    _init();
    
    int ret = main(argc, argv, kapi);
    
    kapi->sys_exit(ret);
    while (1) {}
}
