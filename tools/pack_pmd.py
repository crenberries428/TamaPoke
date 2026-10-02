#!/usr/bin/env python3
"""PMD sprite packer (SpriteCollab) for the main screen.

Generates /mons/pNNN.bin (and shiny psNNN.bin) in multi-action TPK2 format:

  char[4] "TPK2"
  u8  nActs
  u16 palCount
  u16 pal[palCount]                  (RGB565)
  per action:
    u8 id, u8 w, u8 h, u8 nFrames
    u16 ms[nFrames]
    u8 data[w*h*nFrames]             (indices, 0xFF transparent)

Actions: 0 Idle, 1 WalkL, 2 WalkR, 3 Sleep, 4 Eat, 5 Hurt, 6 Attack,
7 Pose, 8 Hop, 9 Nod, 10 DeepBreath, 11 Sit. Missing ones are skipped.

  python3 tools/pack_pmd.py             # the whole dex, normal + shiny
  python3 tools/pack_pmd.py kanto       # only one region (johto, hoenn)
  python3 tools/pack_pmd.py 7 25        # specific dex numbers
  python3 tools/pack_pmd.py normal 1 4  # normals only

Requires Pillow: pip3 install Pillow
"""
import os
import struct
import sys
import subprocess
import xml.etree.ElementTree as ET
from PIL import Image

# Kept in step with dex.h rather than hardcoded, the same way gen_moves.py is.
import re as _re
_dexh = open(os.path.join(os.path.dirname(__file__), '..', 'dex.h')).read()
DEX_COUNT = int(_re.search(r'#define DEX_COUNT (\d+)', _dexh).group(1))

OUT = os.path.join(os.path.dirname(__file__), 'sdcard', 'mons')
CACHE = os.path.join(os.path.dirname(__file__), 'pmd_cache')
BASE = 'https://raw.githubusercontent.com/PMDCollab/SpriteCollab/master/sprite'
SLOW = 1.4          # the original PMD pace feels fast on the tamagotchi
MIN_MS = 70
ALPHA_T = 128

# (id, action name, sheet row) -- row None = 0 if there is only one
# sheet directions: 0 down, 2 RIGHT, 6 LEFT (verified on the board)
ACTIONS = [
    (0, 'Idle', 0),
    (1, 'Walk', 6),   # left
    (2, 'Walk', 2),   # right
    (3, 'Sleep', 0),
    (4, 'Eat', 0),
    (5, 'Hurt', 0),
    (6, 'Attack', 0),
    (7, 'Pose', 0),
    (8, 'Hop', 0),
    (9, 'Nod', 0),
    (10, 'DeepBreath', 0),
    (11, 'Sit', 0),
]


def fetch(url, dest):
    """curl, not urllib. The python.org macOS build ships without a usable CA
    bundle, so urlopen dies with CERTIFICATE_VERIFY_FAILED on every request --
    which this reported as 'no AnimData.xml', i.e. as if the sprite did not
    exist. gen_avatars.py and gen_dex_data.py hit the same wall."""
    if os.path.exists(dest):
        return True
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    r = subprocess.run(['curl', '-fsSL', '--retry', '2', '-A', 'Mozilla/5.0',
                        '-o', dest, url], capture_output=True)
    if r.returncode != 0:
        if os.path.exists(dest):
            os.remove(dest)
        return False
    return True


def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)


def load_animdata(folder):
    path = os.path.join(folder, 'AnimData.xml')
    anims = {}
    tree = ET.parse(path)
    for a in tree.getroot().find('Anims'):
        name = a.find('Name').text
        if a.find('FrameWidth') is None:
            # alias (CopyOf): points to another animation
            copy = a.find('CopyOf')
            if copy is not None:
                anims[name] = ('copy', copy.text)
            continue
        anims[name] = (int(a.find('FrameWidth').text), int(a.find('FrameHeight').text),
                       [int(d.text) for d in a.find('Durations')], name)
    # resolve alias (the PNG is that of the original animation)
    for k, v in list(anims.items()):
        if isinstance(v, tuple) and v[0] == 'copy':
            anims[k] = anims.get(v[1])
    return anims


def pack(dexnum, shiny=False):
    sub = '/0000/0001' if shiny else ''
    folder = os.path.join(CACHE, f'{dexnum:04d}{"s" if shiny else ""}')
    base = f'{BASE}/{dexnum:04d}{sub}'
    if not fetch(f'{base}/AnimData.xml', os.path.join(folder, 'AnimData.xml')):
        raise RuntimeError('no AnimData.xml')
    anims = load_animdata(folder)

    colmap, pal = {}, []
    packed = []
    for aid, name, row in ACTIONS:
        if name not in anims or anims[name] is None:
            continue
        fw, fh, durs, srcname = anims[name]
        png = os.path.join(folder, f'{srcname}-Anim.png')
        if not fetch(f'{base}/{srcname}-Anim.png', png):
            continue
        im = Image.open(png).convert('RGBA')
        rows = im.size[1] // fh
        r = row if rows > row else 0
        nf = min(len(durs), im.size[0] // fw, 24)
        data = bytearray()
        for i in range(nf):
            fr = im.crop((i * fw, r * fh, (i + 1) * fw, (r + 1) * fh))
            for px in fr.getdata():
                if px[3] < ALPHA_T:
                    data.append(0xFF)
                    continue
                k = px[:3]
                if k not in colmap:
                    if len(pal) >= 255:
                        # nearest (rare in PMD, short palettes)
                        k2 = min(colmap, key=lambda c: sum((a-b)**2 for a, b in zip(c, k)))
                        colmap[k] = colmap[k2]
                    else:
                        colmap[k] = len(pal)
                        pal.append(k)
                data.append(colmap[k])
        ms = [max(MIN_MS, round(d * 1000 / 60 * SLOW)) for d in durs[:nf]]
        packed.append((aid, fw, fh, nf, ms, bytes(data)))

    if not any(p[0] == 0 for p in packed):
        raise RuntimeError('no Idle')

    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, f'p{"s" if shiny else ""}{dexnum:03d}.bin')
    with open(path, 'wb') as f:
        f.write(b'TPK2')
        f.write(struct.pack('<BH', len(packed), len(pal)))
        for r, g, b in pal:
            f.write(struct.pack('<H', rgb565(r, g, b)))
        for aid, fw, fh, nf, ms, data in packed:
            f.write(struct.pack('<4B', aid, fw, fh, nf))
            f.write(struct.pack(f'<{nf}H', *ms))
            f.write(data)
    kb = os.path.getsize(path) / 1024
    print(f"  -> p{'s' if shiny else ''}{dexnum:03d}.bin: {len(packed)} actions, "
          f"{len(pal)} colores, {kb:.0f} KB")


if __name__ == '__main__':
    args = sys.argv[1:]
    solo_normal = 'normal' in args
    # The whole dex by default, not the old hardcoded 151. A region is easy to
    # ask for on its own, since the fetch is long and most people want Kanto:
    #   python3 tools/pack_pmd.py kanto
    # Derived from dex_data.py rather than written out again -- this dict
    # stopped at Hoenn while dex.h was already 493, so `pack_pmd.py sinnoh`
    # was not a command that existed.
    import sys as _s
    _s.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from dex_data import REGIONS as _DR
    span = {name.lower(): (lo, hi) for name, lo, hi, _st in _DR}
    picked = [span[a] for a in args if a in span]
    nums = [int(a) for a in args if a.isdigit()]
    if not nums:
        if picked:
            nums = [d for lo, hi in picked for d in range(lo, hi + 1)]
        else:
            nums = list(range(1, DEX_COUNT + 1))
    fallos = []
    for n in nums:
        for sh in ([False] if solo_normal else [False, True]):
            try:
                print(f"#{n:03d}{' shiny' if sh else ''}")
                pack(n, sh)
            except Exception as e:
                print(f"  FALLO: {e}")
                fallos.append((n, sh))
    print(f"FAILURES: {fallos}" if fallos else "ALL PACKED")
