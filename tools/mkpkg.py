#!/usr/bin/env python3
# ======================================================================== #
#  mkpkg.py — OS32 パッケージ (.pkg) 作成ツール
#
#  使用法:
#    python3 tools/mkpkg.py --defs tools/package_defs.yaml --output packages/
#    python3 tools/mkpkg.py --name edit --version 1 --lzss \
#        /usr/bin/edit=programs/edit.bin -o EDIT.PKG
#
#  PKGフォーマット:
#    [PKGヘッダ 32B] [ファイルテーブル] [終端 1B] [データ部 (LZSS or 無圧縮)]
# ======================================================================== #

import sys
import os
import struct
import glob
import argparse

# LZSS定数 (lib/lzss.c と同一)
N = 4096
F = 18
THRESHOLD = 2

def lzss_encode(in_data):
    """LZSS圧縮 (ハッシュチェイン高速版、lzss_pack.py互換出力)"""
    out_data = bytearray()
    out_data.extend(struct.pack('<I', len(in_data)))

    text_buf = bytearray(N + F - 1)
    for i in range(N):
        text_buf[i] = 0x20

    in_len = len(in_data)
    src_p = 0
    r = N - F

    flags = 0
    flag_pos = 0
    code_buf = bytearray(17)
    code_buf_ptr = 1

    # ハッシュチェイン (高速マッチング用)
    HASH_SIZE = 4096
    MAX_CHAIN = 64  # チェインの最大探索深度
    hash_head = [-1] * HASH_SIZE   # hash -> 最新のリングバッファ位置
    hash_prev = [-1] * N           # リングバッファ位置 -> 前の同ハッシュ位置

    def calc_hash(a, b, c):
        return ((a << 5) ^ (b << 3) ^ c) & (HASH_SIZE - 1)

    def insert_hash(pos, byte_val):
        """現在位置をハッシュテーブルに登録 (3バイト先読み不可の場合)"""
        # 3バイトハッシュが計算できない場合は挿入のみ
        pass

    while src_p < in_len:
        match_pos = 0
        match_len = 0

        # ハッシュチェインで候補を検索
        if src_p + 2 < in_len:
            h = calc_hash(in_data[src_p], in_data[src_p + 1], in_data[src_p + 2])
            chain_pos = hash_head[h]
            chain_count = 0

            while chain_pos >= 0 and chain_count < MAX_CHAIN:
                # リングバッファのどの距離にあるか確認
                dist = (r - chain_pos) % N
                if dist == 0 or dist > N:
                    break

                # マッチ長を計測
                l = 0
                while l < F and src_p + l < in_len and text_buf[(chain_pos + l) % N] == in_data[src_p + l]:
                    l += 1
                if l > match_len:
                    match_len = l
                    match_pos = chain_pos
                    if match_len == F:
                        break

                chain_pos = hash_prev[chain_pos]
                chain_count += 1

        if match_len <= THRESHOLD:
            match_len = 1
            flags |= (1 << flag_pos)
            code_buf[code_buf_ptr] = in_data[src_p]
            code_buf_ptr += 1
        else:
            code_buf[code_buf_ptr] = match_pos & 0xFF
            code_buf_ptr += 1
            code_buf[code_buf_ptr] = ((match_pos >> 4) & 0xF0) | (match_len - (THRESHOLD + 1))
            code_buf_ptr += 1

        flag_pos += 1
        if flag_pos == 8:
            code_buf[0] = flags
            out_data.extend(code_buf[:code_buf_ptr])
            flags = 0
            flag_pos = 0
            code_buf_ptr = 1

        for i in range(match_len):
            if src_p < in_len:
                text_buf[r] = in_data[src_p]
                # ハッシュテーブルに挿入 (3バイト先読み可能なら)
                if src_p + 2 < in_len:
                    ih = calc_hash(in_data[src_p], in_data[src_p + 1], in_data[src_p + 2])
                    hash_prev[r] = hash_head[ih]
                    hash_head[ih] = r
                r = (r + 1) % N
                src_p += 1

    if flag_pos > 0:
        code_buf[0] = flags
        out_data.extend(code_buf[:code_buf_ptr])

    return bytes(out_data)


# PKGヘッダ定数
PKG_MAGIC = b'PKG1'
PKG_HEADER_SIZE = 32
PKG_FLAG_LZSS = 0x01
PKG_TYPE_FILE = 0
PKG_TYPE_DIR  = 1

# KAPI_VERSION を os32_kapi_shared.h から読み取る
def read_kapi_version(base_dir):
    """include/os32_kapi_shared.h から KAPI_VERSION を取得"""
    path = os.path.join(base_dir, 'include', 'os32_kapi_shared.h')
    try:
        with open(path, 'r') as f:
            for line in f:
                if '#define' in line and 'KAPI_VERSION' in line:
                    parts = line.split()
                    if len(parts) >= 3:
                        return int(parts[2])
    except (IOError, ValueError):
        pass
    return 0


def build_pkg(name, version, files, use_lzss, kapi_ver):
    """
    PKGファイルを構築する

    files: [(guest_path, host_path), ...] のリスト
    戻り値: bytes (PKGファイル全体)
    """
    # ファイルデータ連結
    raw_data = bytearray()
    entries = []

    # ディレクトリを自動収集
    dirs_seen = set()
    for guest_path, _ in files:
        parts = guest_path.split('/')
        for i in range(1, len(parts)):
            d = '/'.join(parts[:i])
            if d and d != '/' and d not in dirs_seen:
                dirs_seen.add(d)

    # ディレクトリエントリ
    sorted_dirs = sorted(dirs_seen)
    for d in sorted_dirs:
        entries.append((d, 0, PKG_TYPE_DIR))

    # ファイルエントリ
    for guest_path, host_path in files:
        if not os.path.isfile(host_path):
            print(f"WARNING: {host_path} not found, skipping", file=sys.stderr)
            continue
        with open(host_path, 'rb') as f:
            data = f.read()
        entries.append((guest_path, len(data), PKG_TYPE_FILE))
        raw_data.extend(data)

    # LZSS圧縮
    orig_size = len(raw_data)
    if use_lzss and orig_size > 0:
        compressed = lzss_encode(raw_data)
        flags = PKG_FLAG_LZSS
        data_part = compressed
    else:
        flags = 0
        data_part = bytes(raw_data)

    # ファイルテーブル構築
    file_table = bytearray()
    entry_count = 0
    for path, size, ftype in entries:
        path_bytes = path.encode('utf-8')
        path_len = len(path_bytes)
        if path_len > 255:
            path_len = 255
            path_bytes = path_bytes[:255]
        file_table.append(path_len)
        file_table.extend(path_bytes)
        file_table.extend(struct.pack('<I', size))
        file_table.append(ftype)
        entry_count += 1

    # 終端マーカー
    file_table.append(0x00)

    # PKGヘッダ (32バイト)
    name_bytes = name.encode('utf-8')[:8].ljust(8, b'\x00')
    header = bytearray(PKG_HEADER_SIZE)
    header[0:4] = PKG_MAGIC
    header[4:12] = name_bytes
    header[12] = version & 0xFF
    header[13] = flags & 0xFF
    struct.pack_into('<H', header, 14, kapi_ver & 0xFFFF)
    struct.pack_into('<H', header, 16, entry_count & 0xFFFF)
    struct.pack_into('<I', header, 18, orig_size)
    struct.pack_into('<I', header, 22, len(data_part))
    # header[26:32] = 予約 (0x00)

    return bytes(header) + bytes(file_table) + data_part


def build_from_yaml(yaml_path, output_dir, base_dir):
    """package_defs.yaml からパッケージを一括生成"""
    # 簡易YAMLパーサー (PyYAML不要)
    packages = parse_simple_yaml(yaml_path)
    kapi_ver = read_kapi_version(base_dir)

    os.makedirs(output_dir, exist_ok=True)

    for pkg_name, pkg_def in packages.items():
        pkg_type = pkg_def.get('type', 'package')
        version = int(pkg_def.get('version', 1))
        use_lzss = pkg_def.get('lzss', True)
        files = []

        for fdef in pkg_def.get('files', []):
            host = fdef.get('host', '')
            guest = fdef.get('guest', '')

            # Glob展開
            if '*' in host:
                host_base = os.path.join(base_dir, os.path.dirname(host))
                pattern = os.path.join(base_dir, host)
                for match in sorted(glob.glob(pattern)):
                    fname = os.path.basename(match)
                    fguest = guest.rstrip('/') + '/' + fname
                    files.append((fguest, match))
            else:
                host_full = os.path.join(base_dir, host)
                if guest.endswith('/'):
                    guest = guest + os.path.basename(host)
                files.append((guest, host_full))

        if not files:
            print(f"  {pkg_name}: no files, skipping")
            continue

        out_name = pkg_name.upper() + '.PKG'
        out_path = os.path.join(output_dir, out_name)

        pkg_data = build_pkg(pkg_name, version, files, use_lzss, kapi_ver)
        with open(out_path, 'wb') as f:
            f.write(pkg_data)

        total_files = sum(1 for _, _, t in [] if t == 0)
        # リアルなファイル数カウント
        nfiles = len([f for f in files if os.path.isfile(f[1])])
        ratio = ''
        if use_lzss and len(pkg_data) > PKG_HEADER_SIZE:
            orig = sum(os.path.getsize(f[1]) for _, f1 in [] if os.path.isfile(f1))
            orig = sum(os.path.getsize(h) for _, h in files if os.path.isfile(h))
            if orig > 0:
                ratio = f' ({len(pkg_data)*100//orig}%)'
        print(f"  {out_name}: {nfiles} files, {len(pkg_data)} bytes{ratio}")


def parse_simple_yaml(path):
    """
    簡易YAMLパーサー (PyYAML不要)
    対応構造:
      pkg_name:
        key: value
        files:
          - host: path
            guest: path
    """
    packages = {}
    current_pkg = None
    current_file = None
    in_files = False

    with open(path, 'r') as f:
        for line in f:
            stripped = line.rstrip('\n')
            if not stripped or stripped.lstrip().startswith('#'):
                continue

            indent = len(stripped) - len(stripped.lstrip())
            content = stripped.strip()

            # トップレベル (indent 0): パッケージ名
            if indent == 0 and content.endswith(':'):
                current_pkg = content[:-1].strip()
                packages[current_pkg] = {'files': []}
                current_file = None
                in_files = False

            # パッケージ属性 (indent 2)
            elif indent == 2 and current_pkg:
                if content == 'files:':
                    in_files = True
                    current_file = None
                elif ':' in content:
                    k, v = content.split(':', 1)
                    k = k.strip()
                    v = v.strip().strip('"').strip("'")
                    if v.lower() == 'true':
                        v = True
                    elif v.lower() == 'false':
                        v = False
                    packages[current_pkg][k] = v
                    in_files = False

            # ファイルリスト (indent 4): "- host: ..."
            elif indent == 4 and current_pkg and in_files:
                if content.startswith('- host:'):
                    host_val = content[len('- host:'):].strip().strip('"').strip("'")
                    current_file = {'host': host_val, 'guest': ''}
                    packages[current_pkg]['files'].append(current_file)

            # ファイル属性 (indent 6): "guest: ..."
            elif indent == 6 and current_file is not None:
                if content.startswith('guest:'):
                    current_file['guest'] = content[len('guest:'):].strip().strip('"').strip("'")

    return packages


def main():
    parser = argparse.ArgumentParser(description='OS32 Package (.pkg) Builder')
    parser.add_argument('--defs', help='Package definitions YAML file')
    parser.add_argument('--output', '-o', default='packages/',
                        help='Output directory or file')
    parser.add_argument('--name', help='Package name (single pkg mode)')
    parser.add_argument('--version', type=int, default=1,
                        help='Package version')
    parser.add_argument('--lzss', action='store_true',
                        help='Enable LZSS compression')
    parser.add_argument('--base', default='.',
                        help='Base directory for host paths')
    parser.add_argument('files', nargs='*',
                        help='guest=host file mappings (single pkg mode)')
    args = parser.parse_args()

    if args.defs:
        print(f"Building packages from {args.defs}...")
        build_from_yaml(args.defs, args.output, args.base)
    elif args.name and args.files:
        kapi_ver = read_kapi_version(args.base)
        files = []
        for mapping in args.files:
            guest, host = mapping.split('=', 1)
            files.append((guest, host))
        pkg_data = build_pkg(args.name, args.version, files,
                             args.lzss, kapi_ver)
        with open(args.output, 'wb') as f:
            f.write(pkg_data)
        print(f"Created {args.output}: {len(pkg_data)} bytes")
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
