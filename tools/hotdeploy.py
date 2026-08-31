#!/usr/bin/env python3
"""hotdeploy.py — 再起動なしでバイナリ 1 本を実機へ送り込む

NP21/W 内蔵 aidebug の POST /api/mem でゲストのステージングバッファへ
直接バイト列を書き、rshell の hotdeploy コマンドでファイル化させる。
シリアルを通さないので hex 2 倍化もエミュレート速度も効かない。

設計: docs/tasks/hotdeploy/DESIGN.md (案 A)

使い方:
    python3 tools/hotdeploy.py apps/hello32/hello32.bin
    python3 tools/hotdeploy.py apps/hello32/hello32.bin /usr/bin/hello32.bin

ゲストパスを省略すると層別マニフェストから解決する。
"""

import json
import os
import re
import sys
import time
import urllib.parse
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                'np21w_mcp'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import np21w_client as emu          # noqa: E402
from nhd_deploy import resolve_guest_path   # noqa: E402

# aidebug_server.cpp の AIDEBUG_MAX_BODY は 65536。hex で 2 倍になるので
# 1 回に送れる生バイトは 32KB まで。余裕を見て 16KB 刻みにする。
CHUNK = 16 * 1024


def query_staging():
    """ゲストにステージング領域の番地とサイズを聞く"""
    out = emu.post('/api/cmd', 'hotdeploy', timeout=30).decode('utf-8', 'replace')
    m = re.search(r'HOTDEPLOY base=0x([0-9A-Fa-f]+) size=(\d+)', out)
    if not m:
        raise SystemExit(
            "ステージング領域を取得できません。シェルが hotdeploy を持っているか"
            " (make userland/shell.bin 後に配備したか) 確認してください。\n"
            "応答: " + out.strip()[:400])
    return int(m.group(1), 16), int(m.group(2))


# ゲスト側 (kernel/hotdeploy.c hd_path_is_system) と同じ規則。ここでの
# 判定は親切のためで、拒否の権限はゲストが持つ。
SYSTEM_PREFIXES = ('/boot/', '/sys/')


def reject_if_system(guest_path):
    for pre in SYSTEM_PREFIXES:
        if guest_path.startswith(pre):
            raise SystemExit(
                "Error: {} はシステム領域です。ホストからの差し替えは"
                "ユーザーランドに限っています。\n"
                "  カーネル (/boot/vmkernel.lz4) とシステム常駐物 (/sys/*) は"
                "稼働中に書き換えると走っている当人か次回ブートを壊すため、\n"
                "  NHD フル配備で入れ替えてください: os32-cycle deploy"
                .format(guest_path))


def deploy_via_api(data, guest_path):
    """段 3 の経路: POST /api/deploy 一発で送り、ゲストの常駐エージェントに
    ファイル化させる。シェルにコマンドを送らないので、rshell がコマンド待ちの
    まま (あるいはプログラム実行中でも) 差し替えが成立する。

    エージェントが居なければ None を返して呼び出し側にフォールバックさせる。
    """
    st = json.loads(emu.get('/api/deploy', timeout=15).decode('utf-8', 'replace'))
    if not st.get('ok'):
        return None                      # 旧カーネル: 段 2 の経路へ

    if len(data) > st['buf_size']:
        raise SystemExit(
            "Error: {} バイトはステージング窓 {} バイトに収まりません "
            "(MEM_HOTDEPLOY_SIZE を増やすこと)".format(len(data), st['buf_size']))

    crc = zlib.crc32(data) & 0xFFFFFFFF
    q = '/api/deploy?path={}&crc=0x{:08X}'.format(urllib.parse.quote(guest_path), crc)
    r = json.loads(emu.post(q, data.hex(), timeout=120).decode('utf-8', 'replace'))
    if not r.get('ok'):
        raise SystemExit("Error: /api/deploy: {}".format(r))
    print("Staged {} バイト (seq={})".format(r['staged'], r['seq']))

    # ゲストは安全地点でしか処理しない。シリアル待ち・プログラム終了・
    # sys_halt のいずれかを通るまで待つ。
    deadline = time.time() + 60
    while time.time() < deadline:
        st = json.loads(emu.get('/api/deploy', timeout=15).decode('utf-8', 'replace'))
        if st['status'] == 'done':
            return True
        if st['status'] == 'error':
            if st['err'] == 5:
                raise SystemExit("Error: ゲストが拒否 — システム領域への書き込み")
            raise SystemExit("Error: ゲスト側が拒否しました err={} "
                             "(1=length 2=crc 3=write 4=path 5=denied、"
                             "負値は VFS のエラーコード)".format(st['err']))
        time.sleep(0.5)
    raise SystemExit("Error: ゲストが 60 秒以内に処理しませんでした "
                     "(status={})".format(st['status']))


def deploy_via_shell(data, guest_path):
    """段 2 の経路: /api/mem でステージングし rshell の hotdeploy で書かせる。
    エージェントを持たない古いカーネル向けのフォールバック。"""
    base, size = query_staging()
    if len(data) > size:
        raise SystemExit(
            "Error: {} バイトはステージング領域 {} バイトに収まりません "
            "(rshell.c の HD_BUF_SIZE を増やすこと)".format(len(data), size))

    print("Staging: 0x{:08X} ({} バイト空き)".format(base, size))
    sent = 0
    while sent < len(data):
        chunk = data[sent:sent + CHUNK]
        emu.post('/api/mem?addr=0x{:X}&space=linear'.format(base + sent),
                 chunk.hex(), timeout=60)
        sent += len(chunk)
        print("\r  転送 {}/{} バイト".format(sent, len(data)), end='', flush=True)
    print()

    crc = zlib.crc32(data) & 0xFFFFFFFF
    out = emu.post('/api/cmd',
                   'hotdeploy {} {} 0x{:08X}'.format(guest_path, len(data), crc),
                   timeout=120).decode('utf-8', 'replace')
    print(out.strip())
    return 'HOTDEPLOY OK' in out


def push(local_path, guest_path=None):
    if not os.path.isfile(local_path):
        raise SystemExit("Error: {} が見つかりません".format(local_path))

    with open(local_path, 'rb') as f:
        data = f.read()

    if guest_path is None:
        guest_path = resolve_guest_path(local_path)
        if guest_path is None:
            raise SystemExit(
                "マニフェストにマッチしません。ゲストパスを引数で指定してください")
        if guest_path.endswith('/'):
            guest_path += os.path.basename(local_path)
        print("マニフェストから解決: {}".format(guest_path))

    reject_if_system(guest_path)

    ok = deploy_via_api(data, guest_path)
    if ok is None:
        print("常駐エージェントなし — rshell 経由にフォールバック")
        ok = deploy_via_shell(data, guest_path)
    if ok:
        print("HOTDEPLOY OK {} {}".format(guest_path, len(data)))
    return ok


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    ok = push(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
