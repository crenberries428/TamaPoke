#!/usr/bin/env python3
"""Generates /mons/thumbs.bin: 40x40 thumbnails of the whole dex for the gallery.

They are derived from the front frame (Idle, frame 0) of the already packed PMD sprites
(tools/sdcard/mons/pNNN.bin, TPK2 format) -> legal thumbnails (CC BY-NC), same
style as the main screen. TPTH format (little-endian):

  char[4] "TPTH"
  uint16  count
  uint32  offset[count]    (from the start of the file, 1-based: offset[0]=dex 1)
  blobs:  u8 w, u8 h, u8 palCount, u16 pal[palCount], u8 data[w*h] (0xFF transp.)

  python3 tools/make_thumbs.py
"""
import os
import struct

# Kept in step with dex.h rather than hardcoded: a thumbs.bin sized for 151 in a
# 386-species build leaves the Johto and Hoenn galleries showing dex numbers.
import re as _re
_dexh = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'dex.h')).read()
DEX_COUNT = int(_re.search(r'#define DEX_COUNT (\d+)', _dexh).group(1))


DIR = os.path.join(os.path.dirname(__file__), 'sdcard', 'mons')
CELL = 40


def read_pmd_idle_frame0(path):
    """Frame 0 of the Idle action (id 0 = front view) of a PMD TPK2 sprite."""
    with open(path, 'rb') as f:
        buf = f.read()
    if buf[:4] != b'TPK2':
        raise ValueError('magic TPK2')
    nacts = buf[4]
    (palcount,) = struct.unpack_from('<H', buf, 5)
    pal = list(struct.unpack_from(f'<{palcount}H', buf, 7))
    p = 7 + palcount * 2
    for _ in range(nacts):
        aid, w, h, nf = buf[p], buf[p + 1], buf[p + 2], buf[p + 3]
        p += 4 + nf * 2  # header + ms[]
        if aid == 0:     # PMD_IDLE
            return w, h, pal, buf[p:p + w * h]  # frame 0
        p += w * h * nf
    raise ValueError('no Idle action (id 0)')


def shrink(w, h, pal, data):
    # scales to CELL x CELL with nearest neighbour, preserving aspect ratio
    scale = min(CELL / w, CELL / h, 1.0)
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    out = bytearray()
    used = {}
    newpal = []
    for y in range(nh):
        sy = min(h - 1, int(y / scale)) if scale < 1 else y
        for x in range(nw):
            sx = min(w - 1, int(x / scale)) if scale < 1 else x
            idx = data[sy * w + sx]
            if idx == 0xFF:
                out.append(0xFF)
                continue
            c = pal[idx]
            if c not in used:
                used[c] = len(newpal)
                newpal.append(c)
            out.append(used[c])
    return nw, nh, newpal, bytes(out)


def main():
    blobs = []
    missing = []
    for dex in range(1, DEX_COUNT + 1):
        path = os.path.join(DIR, f'p{dex:03d}.bin')
        # Not every species HAS art. This used to assume they all did, which
        # held only while every packed region was 100% -- Unova is the first
        # that is not, and 13 missing files crashed the whole build. An absent
        # species gets an EMPTY blob: it keeps its slot and its offset (the
        # index is positional, so skipping one would shift every species after
        # it), and the gallery falls back to the dex number, which is what it
        # already does for a species whose sprite is not on the card.
        if not os.path.exists(path):
            missing.append(dex)
            blobs.append(struct.pack('<3B', 0, 0, 0))
            continue
        w, h, pal, data = read_pmd_idle_frame0(path)
        nw, nh, npal, ndata = shrink(w, h, pal, data)
        if len(npal) > 255:
            raise ValueError(f'{dex}: palette {len(npal)}')
        blob = struct.pack('<3B', nw, nh, len(npal))
        blob += struct.pack(f'<{len(npal)}H', *npal)
        blob += ndata
        blobs.append(blob)

    head = 4 + 2 + 4 * DEX_COUNT
    offsets, pos = [], head
    for b in blobs:
        offsets.append(pos)
        pos += len(b)

    if missing:
        print('%d species have no sprite here, so no thumbnail: %s' %
              (len(missing), ' '.join(str(d) for d in missing[:16]) +
               ('...' if len(missing) > 16 else '')))

    out = os.path.join(DIR, 'thumbs.bin')
    with open(out, 'wb') as f:
        f.write(b'TPTH')
        f.write(struct.pack('<H', DEX_COUNT))
        f.write(struct.pack('<%dI' % DEX_COUNT, *offsets))
        for b in blobs:
            f.write(b)
    print(f"saved {out}: {pos / 1024:.0f} KB, {len(blobs)} thumbnails")


if __name__ == '__main__':
    main()
