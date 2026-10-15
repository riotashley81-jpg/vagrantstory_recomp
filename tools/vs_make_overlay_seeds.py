#!/usr/bin/env python3
"""Build aot/vs_overlay_seeds.json from a capture store and the extracted disc.

The seed list lets a player's own PC compile Vagrant Story's overlays before
they are first visited (tools/vs_overlay_precompile.py reads it). It holds no
game bytes: for every captured overlay page whose bytes are an exact slice of
a disc file, it records which file, which offset, a SHA-256 of the slice, and
the PCs the capture observed. Pages the game rewrites at runtime never match
the disc and are left to the normal runtime capture.

  python tools/vs_make_overlay_seeds.py \
      --captures-dir build/overlay_captures.json.d build-nocode/overlay_captures.json.d \
      --disc-root ../disc/extracted --out aot/vs_overlay_seeds.json
"""
import argparse
import base64
import glob
import hashlib
import json
import os

# Fixed load addresses of the code files, established from captures
# (executed PC minus file offset agrees for every page of each file).
CODE_FILES = {
    # The boot executable: its 2 KiB PS-X EXE header precedes 0x80010000.
    # Only disc-code builds capture it (game-C builds compile it statically).
    'SLUS_010.40': 0x80010000 - 0x800,
    'BATTLE/BATTLE.PRG': 0x80068800,
    'TITLE/TITLE.PRG': 0x80068800,
    'BATTLE/INITBTL.PRG': 0x800F9800,
    'MENU/MAINMENU.PRG': 0x800F9800,
    'GIM/SCREFF2.PRG': 0x800F9800,
}
for _n in '3 4 5 7 8 9 B C D E'.split():
    CODE_FILES['MENU/MENU%s.PRG' % _n] = 0x80102800

LIST_FIELDS = ('executed_pcs', 'dispatch_entry_pcs', 'function_entry_pcs', 'seeds')


def load_captures(directories):
    merged = {}
    paths = [p for d in directories for p in glob.glob(os.path.join(d, '*.json'))]
    for path in sorted(paths):
        with open(path) as f:
            data = json.load(f)
        for cap in data if isinstance(data, list) else [data]:
            key = (cap['load_addr'], cap['size'], cap['bytes_b64'])
            dst = merged.setdefault(key, {'cap': cap, 'lists': {}, 'segs': {}})
            for field in LIST_FIELDS:
                seen = dst['lists'].setdefault(field, {})
                for pc in cap.get(field) or []:
                    seen[pc] = None
            for seg, pcs in (cap.get('dispatch_entry_segments') or {}).items():
                seen = dst['segs'].setdefault(seg, {})
                for pc in pcs:
                    seen[pc] = None
    return merged.values()


def pc_runs(pcs):
    """Sorted PCs as [start, count] runs of consecutive words."""
    values = sorted(int(pc, 16) for pc in pcs)
    runs = []
    for v in values:
        if runs and v == runs[-1][0] + 4 * runs[-1][1]:
            runs[-1][1] += 1
        else:
            runs.append([v, 1])
    return [['0x%08X' % s, n] for s, n in runs]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--captures-dir', required=True, nargs='+',
                    help='one or more overlay_captures.json.d stores')
    ap.add_argument('--disc-root', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    files = {}
    for name in CODE_FILES:
        with open(os.path.join(a.disc_root, *name.split('/')), 'rb') as f:
            files[name] = f.read()

    records = []
    for entry in load_captures(a.captures_dir):
        cap = entry['cap']
        body = base64.b64decode(cap['bytes_b64'])
        load = int(cap['load_addr'], 16)
        for name, base in CODE_FILES.items():
            off = load - base
            if off < 0 or files[name][off:off + len(body)] != body:
                continue
            rec = {
                'file': name,
                'file_offset': off,
                'load_addr': cap['load_addr'],
                'size': cap['size'],
                'guard_bytes': cap.get('guard_bytes', 0),
                'sha256': hashlib.sha256(body).hexdigest(),
            }
            for field in LIST_FIELDS:
                rec[field] = pc_runs(entry['lists'].get(field, {}))
            rec['dispatch_entry_segments'] = {
                seg: pc_runs(pcs) for seg, pcs in entry['segs'].items()}
            records.append(rec)
            break
    records.sort(key=lambda r: (r['file'], int(r['load_addr'], 16), r['sha256']))
    out = {
        'schema': 'vs overlay seeds v1',
        'capture_schema': 'psxrecomp overlay capture v3',
        'records': records,
    }
    with open(a.out, 'w', newline='\n') as f:
        json.dump(out, f, separators=(',', ':'))
        f.write('\n')
    print('%d seed pages from %d files -> %s' % (
        len(records), len({r['file'] for r in records}), a.out))


if __name__ == '__main__':
    main()
