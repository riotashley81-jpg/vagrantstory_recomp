#!/usr/bin/env python3
"""Compile Vagrant Story's overlays from the player's own disc, ahead of play.

Run by the game (src/vs_overlay_precompile.c) from <exe>/overlay_toolchain
with the bundled Python. aot/vs_overlay_seeds.json, staged beside this script
as vs_overlay_seeds.json, names disc-file slices and the PCs known to run in
them; it carries no game bytes. This script reads those slices from the disc
the player launched (disc.cfg), checks each against its SHA-256, writes them
as overlay capture records and hands them to compile_overlays.py with the
compiler the runtime itself would use. Shards land in <exe>/cache, where the
loader picks them up. A marker in the cache skips the work on later launches.

  vs_overlay_precompile.py --exe-dir <dir> --flavor N --arch-abi <os-arch>
                           [--cps] [--disc <cue>]
"""
import argparse
import base64
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SEEDS = os.path.join(HERE, 'vs_overlay_seeds.json')


class Disc:
    """ISO9660 reader for the first data track of a cue/bin."""

    def __init__(self, cue):
        cue_dir = os.path.dirname(os.path.abspath(cue))
        self.bin = None
        with open(cue, encoding='utf-8', errors='replace') as f:
            current = None
            for line in f:
                m = re.match(r'\s*FILE\s+"([^"]+)"', line, re.IGNORECASE)
                if m:
                    current = os.path.join(cue_dir, m.group(1))
                t = re.match(r'\s*TRACK\s+\d+\s+MODE(\d)/(\d+)', line, re.IGNORECASE)
                if t and current:
                    self.bin = current
                    mode, self.sector = int(t.group(1)), int(t.group(2))
                    self.data_off = 0 if self.sector == 2048 else (24 if mode == 2 else 16)
                    break
        if not self.bin:
            raise RuntimeError('no data track in %s' % cue)
        self.f = open(self.bin, 'rb')
        pvd = self.read_sectors(16, 1)
        if pvd[1:6] != b'CD001':
            raise RuntimeError('no ISO9660 volume in %s' % self.bin)
        self.root = self._record(pvd, 156)

    def read_sectors(self, lba, count):
        out = bytearray()
        for i in range(count):
            self.f.seek((lba + i) * self.sector + self.data_off)
            out += self.f.read(2048)
        return bytes(out)

    @staticmethod
    def _record(buf, off):
        length = buf[off]
        lba = struct.unpack_from('<I', buf, off + 2)[0]
        size = struct.unpack_from('<I', buf, off + 10)[0]
        flags = buf[off + 25]
        name_len = buf[off + 32]
        name = buf[off + 33:off + 33 + name_len].decode('ascii', 'replace')
        return {'len': length, 'lba': lba, 'size': size, 'dir': bool(flags & 2),
                'name': name.split(';')[0].upper()}

    def _list(self, rec):
        data = self.read_sectors(rec['lba'], (rec['size'] + 2047) // 2048)
        entries, off = [], 0
        while off < len(data):
            if data[off] == 0:
                off = (off // 2048 + 1) * 2048
                continue
            entries.append(self._record(data, off))
            off += data[off]
        return entries

    def read_file(self, path):
        rec = self.root
        for part in path.upper().split('/'):
            rec = next((e for e in self._list(rec) if e['name'] == part), None)
            if rec is None:
                raise FileNotFoundError(path)
        return self.read_sectors(rec['lba'], (rec['size'] + 2047) // 2048)[:rec['size']]


def expand(runs):
    return ['0x%08X' % (int(start, 16) + 4 * i) for start, n in runs for i in range(n)]


def disc_path(exe_dir, explicit):
    """The disc the game launched: --disc, else what the launcher remembered
    (disc.cfg, then settings.toml [disc] path)."""
    path = explicit
    if not path:
        try:
            with open(os.path.join(exe_dir, 'disc.cfg'), encoding='utf-8') as f:
                path = f.read().strip()
        except OSError:
            pass
    if not path:
        import tomllib
        try:
            with open(os.path.join(exe_dir, 'settings.toml'), 'rb') as f:
                path = tomllib.load(f).get('disc', {}).get('path', '')
        except (OSError, ValueError):
            pass
    if not path:
        raise RuntimeError('no disc path (disc.cfg / settings.toml)')
    return path if os.path.isabs(path) else os.path.join(exe_dir, path)


def compiler_args(exe_dir):
    """The compiler the runtime picks: overlay_toolchain/compiler.txt (gcc)
    when it names an existing compiler, else the bundled tcc."""
    try:
        with open(os.path.join(HERE, 'compiler.txt'), encoding='utf-8') as f:
            gcc = f.readline().strip()
    except OSError:
        gcc = ''
    if gcc and not os.path.isabs(gcc):
        gcc = os.path.join(exe_dir, gcc)
    if gcc and os.path.exists(gcc):
        return 'gcc', ['--compiler', 'gcc', '--gcc', gcc]
    return 'tcc', ['--compiler', 'tcc', '--tcc', os.path.join(HERE, 'tcc', 'tcc.exe')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe-dir', required=True)
    ap.add_argument('--flavor', default='0')
    ap.add_argument('--arch-abi', default='')
    ap.add_argument('--cps', action='store_true',
                    help='the runtime is continuation-passing; shards must match')
    ap.add_argument('--disc', default='')
    a = ap.parse_args()

    exe_dir = os.path.abspath(a.exe_dir)
    cache = os.path.join(exe_dir, 'cache')
    with open(SEEDS, 'rb') as f:
        seeds_raw = f.read()
    seeds = json.loads(seeds_raw)
    compiler, cargs = compiler_args(exe_dir)
    stamp = hashlib.sha256(seeds_raw + compiler.encode() + a.flavor.encode() +
                           a.arch_abi.encode() + (b'cps' if a.cps else b'')).hexdigest()[:16]
    marker = os.path.join(cache, 'vs_precompiled_%s.ok' % stamp)
    if os.path.exists(marker):
        print('vs precompile: already done (%s)' % os.path.basename(marker))
        return 0

    disc = Disc(disc_path(exe_dir, a.disc))
    files, captures, mismatched = {}, [], 0
    for rec in seeds['records']:
        if rec['file'] not in files:
            files[rec['file']] = disc.read_file(rec['file'])
        body = files[rec['file']][rec['file_offset']:rec['file_offset'] + rec['size']]
        if hashlib.sha256(body).hexdigest() != rec['sha256']:
            mismatched += 1
            continue
        captures.append({
            'schema': seeds['capture_schema'],
            'load_addr': rec['load_addr'],
            'size': rec['size'],
            'guard_bytes': rec['guard_bytes'],
            'bytes_b64': base64.b64encode(body).decode('ascii'),
            'executed_pcs': expand(rec['executed_pcs']),
            'dispatch_entry_pcs': expand(rec['dispatch_entry_pcs']),
            'dispatch_entry_segments': {s: expand(r) for s, r in
                                        rec['dispatch_entry_segments'].items()},
            'function_entry_pcs': expand(rec['function_entry_pcs']),
            'seeds': expand(rec['seeds']),
        })
    # Most-run pages first: the battle engine is native long before the
    # rarely used menus, which matters most with gcc's slower compile.
    captures.sort(key=lambda c: -len(c['executed_pcs']))
    if not captures:
        print('vs precompile: no seed page matched this disc (%d mismatched)' % mismatched)
        return 2

    os.makedirs(cache, exist_ok=True)
    seed_captures = os.path.join(cache, 'vs_seed_captures.json')
    with open(seed_captures, 'w') as f:
        json.dump(captures, f)

    env = dict(os.environ)
    # The runtime pins these for its own compiles in the game process's
    # environment; this run reads the seed captures instead.
    env.pop('PSX_OVERLAY_CAPTURES', None)
    env.pop('PSX_OVERLAY_LIVE_AUTOCOMPILE', None)
    env['PSX_OVERLAY_CACHE_DIR'] = cache
    env['PSX_OVERLAY_FLAVOR'] = a.flavor
    if a.arch_abi:
        env['PSX_OVERLAY_ARCH_ABI'] = a.arch_abi
    cmd = [sys.executable, os.path.join(HERE, 'compile_overlays.py'),
           '--captures', seed_captures,
           '--game-toml', os.path.join(exe_dir, 'game.toml'),
           '--recompiler', os.path.join(HERE, 'psxrecomp-game.exe'),
           '--runtime-include', os.path.join(HERE, 'include'),
           '--project-root', HERE,
           '--out-dir', cache] + (['--cps'] if a.cps else []) + cargs
    print('vs precompile: %d pages (%d mismatched) with %s' % (
        len(captures), mismatched, compiler), flush=True)
    rc = subprocess.call(cmd, env=env, cwd=exe_dir)
    if rc == 0:
        with open(marker, 'w') as f:
            f.write('%d pages\n' % len(captures))
        os.remove(seed_captures)
    return rc


if __name__ == '__main__':
    sys.exit(main())
