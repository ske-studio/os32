import re, json

with open("/mnt/c/WATCOM/src/os32_gcc/include/os32_kapi_shared.h", "r", encoding="utf-8") as f:
    text = f.read()

m = re.search(r'typedef struct \{(.*?)\} KernelAPI;', text, re.DOTALL)
if not m:
    print("Failed to find KernelAPI")
    exit(1)

body = m.group(1)
api_list = []

for line in body.split("\n"):
    line = line.strip()
    if not line or line.startswith("/*") or line.startswith("//") or line.startswith("u32 magic;") or line.startswith("u32 version;"):
        continue
    line = line.split("/*")[0].strip()
    if not line: continue
    
    m2 = re.match(r'^(.*?)\s*\(__cdecl\s*\*(.*?)\)\((.*?)\);', line)
    if m2:
        ret_type = m2.group(1).strip()
        name = m2.group(2).strip()
        args_str = m2.group(3).strip()
        
        args = [] if args_str == "void" else [a.strip() for a in args_str.split(",")]
        
        entry = { "name": name, "ret": ret_type, "args": args }
        
        # Mapping specifics
        if name == "mem_alloc": entry["target"] = "exec_heap_alloc"
        elif name == "mem_free": entry["target"] = "exec_heap_free"
        elif name == "dev_get_info": entry["target"] = "dev_api_get_info"
        elif name == "get_tick": entry["body"] = "return tick_count;"
        elif name == "rtc_read": entry["body"] = "rtc_read((RTC_Time *)rtc_time);"
        elif name == "rshell_set_active": entry["body"] = "rshell_active = active;"
        elif name == "file_ls": entry["body"] = "return vfs_ls(path, (vfs_dir_cb)cb, ctx);"
        elif name == "path_parse": entry["body"] = "path_parse(input, (ParsedPath *)result);"
        elif name == "ide_identify": entry["body"] = "return ide_identify(drv, (IdeInfo *)info);"

        api_list.append(entry)

out = {
    "version": 8,
    "includes": [
        "gfx.h", "kbd.h", "exec_heap.h", "kmalloc.h", "paging.h",
        "rtc.h", "exec.h", "fm.h", "np2sysp.h", "ide.h", "path.h",
        "tvram.h", "vfs.h", "ext2.h", "serial.h", "console.h",
        "dev.h", "kcg.h", "sys.h", "kprintf.h", "io.h"
    ],
    "externs": [
        "extern volatile u32 tick_count;"
    ],
    "api": api_list
}

with open("/mnt/c/WATCOM/src/os32_gcc/tools/kapi.json", "w", encoding="utf-8") as f:
    json.dump(out, f, indent=2)

print("Dumped", len(api_list), "APIs")
