#!/usr/bin/env python3
"""
upload.py — OS32へのバイナリファイルアップロード (互換ラッパー)

実体は nhd_deploy.py push に移行済み。
このスクリプトは後方互換性のために残されている。

使い方:
  python3 upload.py <local_file> [remote_name]
"""

import sys
import os
import subprocess

def main():
    if len(sys.argv) < 2:
        print("Usage: {} <local_file> [remote_name]".format(sys.argv[0]))
        print("")
        print("注意: このスクリプトは nhd_deploy.py push の互換ラッパーです。")
        print("  推奨: python3 nhd_deploy.py push [--resolve] <file> [guest]")
        sys.exit(1)

    local_path = sys.argv[1]
    remote_name = sys.argv[2] if len(sys.argv) > 2 else None

    if not os.path.isfile(local_path):
        print("Error: {} not found".format(local_path))
        sys.exit(1)

    # nhd_deploy.py push に委譲
    script_dir = os.path.dirname(os.path.abspath(__file__))
    nhd_deploy = os.path.join(script_dir, 'nhd_deploy.py')

    cmd = [sys.executable, nhd_deploy, 'push']
    if remote_name:
        cmd.extend([local_path, remote_name])
    else:
        cmd.extend(['--resolve', local_path])
    
    sys.exit(subprocess.call(cmd))

if __name__ == '__main__':
    main()
