import glob, os, re

files = glob.glob('programs/*.c') + glob.glob('programs/*/*.c')
for f in files:
    with open(f, 'r', encoding='utf-8') as file:
        content = file.read()
    
    new_content = re.sub(r'void(__cdecl )? main\(KernelAPI \*api\)', r'void\1 main(int argc, char **argv, KernelAPI *api)', content)
    new_content = re.sub(r'void main\( KernelAPI \*api \)', r'void main(int argc, char **argv, KernelAPI *api)', new_content)
    
    if new_content != content:
        with open(f, 'w', encoding='utf-8') as file:
            file.write(new_content)
        print(f"Updated {f}")
