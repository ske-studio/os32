#!/usr/bin/env python3
"""Narrow NP21/W byte transformation and OFFLINE preparation only.

No emulator/credential imports, shell, network, process control or live writer.
--apply requires --output: writes only a new private offline bundle, never the
input snapshot. Live apply is blocked until a trustworthy exit verifier and
local emu_agent orchestration are integrated. See os32-emu-config/SKILL.md.
"""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import uuid

# Proven by docs/tasks/gui/TASK_H3_cirrus.md §0 and TASKS.md 2026-09-06
# (WAB OFF recheck). Other WAB fields/board IDs have no approved values here.
#
# USEPEGCP is the PEGC gate, proven by np21w-src src/win9x/ini.cpp:687
# (PFVAL("USEPEGCP", PFTYPE_BOOL, &np2cfg.usepegcplane), same s_IniItems[]
# table and [NekoProject21] section as USEGD5430) and src/io/pegc.c:375
# (pegc.enable = np2cfg.usepegcplane), which mem/memvga.c reads on every
# PEGC VRAM path. pc_model is NOT the gate: OS32's pegc_probe() reads BIOS
# work area 0x045C bit6 / 0x0597 bit2, and both are already set with
# pc_model=VX (measured on the guest 2026-09-09).
#
# All fields are required even when changing one, so a partial or unknown
# backend configuration fails closed. NP21/W's initsave writes the whole
# s_IniItems[] table, so an ini it produced always carries them.
#
# EXMEMORY は ini では `ExMemory` と混在ケースで書かれる。transform はキーを
# 大文字化して照合し、書き換えるのは値だけなので ini 側の綴りは保たれる
# (既存の 3 キーはたまたま全大文字だったのでこの区別が要らなかった)。
# MB 単位の拡張メモリ (win9x/ini.cpp:477, PFTYPE_UINT16。
# SUPPORT_LARGE_MEMORY 無効時は UINT8、どちらも MEMORY_MAXSIZE でクランプ)。
# ブートローダが 1MB から 512KB 刻みで実測するので (boot/loader_fat.asm:248)、
# ゲストの総容量はこの値で決まる。値は 2 つだけ通す:
#   16 = 現行構成 (ゲスト 15360KB = 15MB。実測 2026-09-09)
#    7 = ゲスト 8MB。memory_boot の legacy フォールバックを通す
#    8 = ゲスト 9MB。PEGC (640x480) の 300KB 予約がアプリ帯の外に出る最小構成
# 8MB は CUI の最低動作環境であって GUI の最低要件ではない
# (INSTALL.md / docs/02_memory.md / tasks/gui/DESIGN.md)。
ALLOWED = {'USEGD5430': ('true', 'false'), 'GD5430TYPE': ('91',),
           'USEPEGCP': ('true', 'false'), 'EXMEMORY': ('7', '8', '16')}
SECTION = b'nekoproject21'
LIMIT = 4 * 1024 * 1024


class IniError(ValueError):
    """A safe, content-free diagnostic suitable for operator output."""


def _changes(changes):
    if not isinstance(changes, dict) or not changes:
        raise IniError('nonempty allowlisted changes required')
    for key, value in changes.items():
        if key not in ALLOWED or value not in ALLOWED[key]:
            raise IniError('unsupported backend field or value')


def transform(raw, changes):
    """Pure bytes -> (bytes, changed-field lines); never decode opaque content.

    ASCII-compatible input only (including CP932 and UTF-8 BOM). Preserve line
    terminators, spacing, comments and all unrelated bytes. Reject UTF-16/32,
    NUL and ambiguous target section/fields. Require both proven fields even
    when changing one, so a partial/unknown backend configuration fails closed.
    """
    _changes(changes)
    if not isinstance(raw, bytes) or len(raw) > LIMIT or b'\0' in raw:
        raise IniError('unsupported snapshot bytes or size')
    if raw.startswith((b'\xff\xfe', b'\xfe\xff')):
        raise IniError('unsupported encoding')
    parts = re.split(b'(\r\n|\r|\n)', raw)
    section = None
    sections = 0
    found = {}
    for i in range(0, len(parts), 2):
        line = parts[i]
        bom = b'\xef\xbb\xbf' if i == 0 and line.startswith(b'\xef\xbb\xbf') else b''
        body = line[len(bom):]
        stripped = body.strip(b' \t')
        if not stripped or stripped.startswith((b';', b'#')):
            continue
        if stripped.startswith(b'['):
            match = re.fullmatch(rb'\[([^\]\r\n]+)\][ \t]*(?:[;#].*)?', stripped)
            if not match:
                raise IniError('malformed section')
            section = match[1].strip().lower()
            if section == SECTION:
                sections += 1
            continue
        if section != SECTION:
            continue
        match = re.fullmatch(rb'([ \t]*)([A-Za-z0-9_]+)([ \t]*=[ \t]*)([^;#]*)([;#].*)?', body)
        if not match:
            raise IniError('malformed target section assignment')
        key = match[2].decode('ascii').upper()
        if key not in ALLOWED:
            continue
        if key in found:
            raise IniError('duplicate backend field')
        token = match[4].rstrip(b' \t')
        if token not in tuple(v.encode('ascii') for v in ALLOWED[key]):
            raise IniError('unsupported existing backend value')
        start = len(bom) + match.start(4)
        found[key] = (i, start, start + len(token), token.decode('ascii'))
    if sections != 1 or set(found) != set(ALLOWED):
        raise IniError('missing or duplicate target section/backend fields')
    diff = []
    for key in ALLOWED:
        if key not in changes:
            continue
        i, start, end, old = found[key]
        new = changes[key]
        if old != new:
            parts[i] = parts[i][:start] + new.encode('ascii') + parts[i][end:]
            diff.append(f'{key}: {old} -> {new}')
    return b''.join(parts), diff


@contextmanager
def _directory(path):
    """Anchor every component with directory FDs; reject all symlinks."""
    path = Path(path)
    if '..' in path.parts:
        raise IniError('parent traversal is not supported')
    path = path.absolute()
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parts[1:]:
            try:
                nxt = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            except OSError as exc:
                raise IniError('directory unavailable or symlink') from exc
            os.close(fd)
            fd = nxt
        yield fd
    finally:
        os.close(fd)


def _signature(info):
    return [info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns]


def _read(fd, name):
    try:
        handle = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
    except OSError as exc:
        raise IniError('file unavailable or symlink') from exc
    before = os.fstat(handle)
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
        os.close(handle)
        raise IniError('single-link regular file required')
    with os.fdopen(handle, 'rb') as stream:
        data = stream.read(LIMIT + 1)
        after = os.fstat(stream.fileno())
    if len(data) > LIMIT or _signature(before) != _signature(after):
        raise IniError('snapshot oversized or changed during read')
    return data, _signature(after)


def read_snapshot(path):
    path = Path(path)
    with _directory(path.parent) as fd:
        return _read(fd, path.name)[0]


def _new_file(fd, name, data):
    handle = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o600, dir_fd=fd)
    with os.fdopen(handle, 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def _atomic(fd, name, data, expected=None):
    """Offline-only writer. Caller holds bundle lock; compare before replace."""
    temp = '.pending-' + uuid.uuid4().hex
    _new_file(fd, temp, data)
    try:
        if expected is not None:
            if _read(fd, name) != expected:
                raise IniError('intervening modification; refusing replacement')
        else:
            try:
                os.stat(name, dir_fd=fd, follow_symlinks=False)
            except FileNotFoundError:
                pass
            else:
                raise IniError('offline destination already exists')
        os.replace(temp, name, src_dir_fd=fd, dst_dir_fd=fd)
        os.fsync(fd)
        actual, signature = _read(fd, name)
        if actual != data:
            raise IniError('readback verification failed; retain backup')
        return signature
    finally:
        try:
            os.unlink(temp, dir_fd=fd)
        except FileNotFoundError:
            pass


def _hash(data):
    return hashlib.sha256(data).hexdigest()


def prepare(snapshot, output, changes):
    """Create a unique offline bundle; NEVER write the supplied snapshot.

    Failure leaves any created backup in place for diagnosis. original.bin is
    fsynced and verified before prepared.bin is atomically published. Receipt
    is published last; incomplete bundles cannot be restored by this tool.
    """
    original = read_snapshot(snapshot)
    candidate, _ = transform(original, changes)
    name = 'np21w-offline-' + uuid.uuid4().hex
    with _directory(output) as parent:
        os.mkdir(name, 0o700, dir_fd=parent)
        fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            _new_file(fd, 'original.bin', original)
            os.fsync(fd)
            if _read(fd, 'original.bin')[0] != original:
                raise IniError('backup readback failed')
            signature = _atomic(fd, 'prepared.bin', candidate)
            receipt = {'format': 1, 'changes': changes, 'original': _hash(original),
                       'prepared': _hash(candidate), 'signature': signature}
            _atomic(fd, 'receipt.json', json.dumps(receipt, sort_keys=True).encode('ascii'))
            os.fsync(parent)
        finally:
            os.close(fd)
    return Path(output).absolute() / name


def restore(bundle, apply=False):
    """Restore ONLY prepared.bin in an offline bundle; default is field diff.

    Validate hashes, exact prepared file identity/timestamps and pure transform
    provenance. No receipt-specified paths or shell commands are accepted.
    This is not a live restore API or a stopped-emulator assertion.
    """
    with _directory(bundle) as fd:
        fcntl.flock(fd, fcntl.LOCK_EX)
        try:
            receipt = json.loads(_read(fd, 'receipt.json')[0])
            if set(receipt) != {'format', 'changes', 'original', 'prepared', 'signature'} or receipt['format'] != 1:
                raise IniError('invalid offline receipt')
            original, _ = _read(fd, 'original.bin')
            current, signature = _read(fd, 'prepared.bin')
            expected, diff = transform(original, receipt['changes'])
            if (current != expected or _hash(original) != receipt['original'] or
                    _hash(current) != receipt['prepared'] or signature != receipt['signature']):
                raise IniError('intervening modification; refusing restore')
        except (KeyError, TypeError, ValueError, UnicodeError) as exc:
            raise IniError('invalid or modified offline bundle; refusing restore') from exc
        reverse = []
        for line in diff:
            key, values = line.split(': ')
            old, new = values.split(' -> ')
            reverse.append(f'{key}: {new} -> {old}')
        if apply:
            _atomic(fd, 'prepared.bin', original, (current, signature))
        return reverse


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    prep = sub.add_parser('prepare', help='dry-run an offline snapshot; never writes input')
    prep.add_argument('snapshot')
    prep.add_argument('--set', action='append', required=True, dest='settings', metavar='KEY=VALUE')
    prep.add_argument('--output', help='existing parent directory for a NEW offline bundle')
    prep.add_argument('--apply', action='store_true', help='create offline bundle (requires --output)')
    rest = sub.add_parser('restore', help='restore prepared.bin inside an offline bundle only')
    rest.add_argument('bundle')
    rest.add_argument('--apply', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.command == 'restore':
            diff = restore(args.bundle, args.apply)
        else:
            if args.apply and not args.output:
                raise IniError('live apply blocked: trustworthy stopped-emulator verifier unavailable; use --output for offline preparation')
            changes = {}
            for setting in args.settings:
                key, sep, value = setting.partition('=')
                if not sep or key in changes:
                    raise IniError('invalid or repeated assignment')
                changes[key] = value
            _changes(changes)
            if args.apply:
                bundle = prepare(args.snapshot, args.output, changes)
                _, diff = transform(read_snapshot(bundle / 'original.bin'), changes)
                print('offline bundle: ' + str(bundle))
            else:
                _, diff = transform(read_snapshot(args.snapshot), changes)
        for line in diff:
            print(line)
        return 0
    except IniError as exc:
        print('error: ' + str(exc), file=sys.stderr)
        return 2
    except OSError:
        print('error: offline file operation failed; retain any backup', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
