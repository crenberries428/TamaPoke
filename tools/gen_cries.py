#!/usr/bin/env python3
"""Real Gen 1 cries: pret/pokered audio/sfx/cry*_1.asm + data/pokemon/cries.asm -> cries.h

    python3 tools/gen_cries.py

A Gen 1 cry is not a recording. It is a three-channel sequence (two pulse
voices and a noise voice) of `square_note` / `noise_note` commands, and there
are only ~38 of them. Each species reuses one and bends it with two bytes from
CryData: a PITCH added to every pulse frequency, and a LENGTH that scales how
long the pulse notes last. That is why Gen 1 cries sound related.

Three things that are easy to get wrong, all read from audio/engine_1.asm:

  * CryData is indexed by the INTERNAL species id (Rhydon is 1), not by dex
    number. data/pokemon/dex_order.asm maps one to the other.
  * Pitch is added to the LOW byte of the 11-bit frequency with a carry into the
    high byte -- i.e. freq + pitch, unsigned, wrapped to 11 bits.
  * Length: pulse note delay = frames * (length + 0x80) / 256. 0x80 is "as
    written". The noise channel ignores the modifier entirely.

Duration of one frame is 1/59.73 s. The duty_cycle_pattern rotates through four
duties every frame on hardware; only the first is kept here, which is judged by
ear (`--wav --demo cry`), like the music.

Gen 2 (152-251) comes from pret/pokecrystal the same way, via gen2(): Crystal's
cries are the same square_note / noise_note vocabulary, but each of the 251
species has its OWN sequence (written with sound_loop / sound_call / sound_jump,
which the interpreter in run_channel() flattens), and the modifiers are 16-bit:
pitch is added to every pulse frequency, length is the channel TEMPO (256 = as
written; Gen 1's byte is stored here as length + 0x80 so one formula serves
both). A channel that says `pitch_offset N` REPLACES the species pitch with N.
`pitch_sweep` has no equivalent in gbsynth and is ignored.

Gen 3+ are sampled audio, not note data, and are NOT handled here.

Source: Nintendo / Game Freak / Creatures, via the pokered disassembly.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'cries.h')
RAW = 'https://raw.githubusercontent.com/pret/pokered/master/'

KANTO = 151
JOHTO_END = 251
RAW2 = 'https://raw.githubusercontent.com/pret/pokecrystal/master/'


def fetch(path, raw=None):
    r = subprocess.run(['curl', '-fsSL', (raw or RAW) + path], capture_output=True)
    if r.returncode != 0:
        raise SystemExit('fetch failed: ' + path)
    return r.stdout.decode('utf-8', 'replace')


def noise_period(nr43):
    """Noise register -> samples between shift-register clocks at 16 kHz."""
    s = (nr43 >> 4) & 15
    r = nr43 & 7
    div = 0.5 if r == 0 else float(r)
    hz = 524288.0 / div / (2 ** (s + 1))
    return max(1, min(255, int(round(16000.0 / hz))))


def parse_channel(text, label):
    """-> [(freq, duty, vol, envDir, envPeriod, frames)] for one channel."""
    # a channel ends at sound_ret, or runs into the next label (cry 02's Ch6)
    m = re.search(re.escape(label) + r':\n(.*?)(?:sound_ret|\n\w+:)', text, re.S)
    if not m:
        return None
    duty = 2
    out = []
    for line in m.group(1).splitlines():
        line = line.split(';')[0].strip()
        if not line:
            continue
        if line.startswith('sound_loop'):
            # `sound_loop N, label` plays the channel N times in all
            out = out * int(line.split()[1].rstrip(','))
        elif line.startswith('duty_cycle_pattern'):
            duty = int(line.split()[1].rstrip(',')) & 3
        elif line.startswith('duty_cycle'):
            duty = int(line.split()[1]) & 3
        elif line.startswith('square_note') or line.startswith('noise_note'):
            a = [int(x) for x in re.findall(r'-?\d+', line)]
            length, vol, fade, freq = a
            if fade < 0:
                d, per = 1, -fade
            else:
                d, per = -1, fade
            if per == 0:
                d = 0
            if line.startswith('noise_note'):
                freq = noise_period(freq)
            out.append((freq, duty, vol, d, per, length + 1))
        else:
            raise SystemExit('unhandled command in %s: %s' % (label, line))
    return out


def parse_event(line, label):
    a = [int(x) for x in re.findall(r'-?\d+', line)]
    length, vol, fade, freq = a
    if fade < 0:
        d, per = 1, -fade
    else:
        d, per = -1, fade
    if per == 0:
        d = 0
    return length, vol, d, per, freq


def run_channel(lines, labels, start, name):
    """Interpret one Crystal cry channel -> (events, pitch_override or -1).

    sound_loop N, L plays the body N times in all; sound_call runs a subroutine
    and returns; sound_jump ends this path; sound_ret ends the channel (or
    returns from a call). Local .labels belong to the preceding global label.
    """
    out = []
    duty = 2
    override = -1
    ip = start
    stack = []
    counters = {}
    steps = 0
    while True:
        steps += 1
        if steps > 20000:
            raise SystemExit('runaway channel ' + name)
        if ip >= len(lines):
            break
        scope, line = lines[ip]
        ip += 1
        if not line or line.endswith(':'):
            continue
        cmd = line.split()[0]
        arg = line[len(cmd):].strip()
        if cmd == 'sound_ret':
            if stack:
                ip = stack.pop()
                continue
            break
        elif cmd == 'sound_loop':
            n, lab = [x.strip() for x in arg.split(',')]
            n = int(n)
            if n == 0:
                raise SystemExit('infinite loop in ' + name)
            key = ip
            c = counters.get(key, n) - 1
            if c > 0:
                counters[key] = c
                ip = labels[lab if not lab.startswith('.') else scope + lab]
            else:
                counters.pop(key, None)
        elif cmd == 'sound_call':
            stack.append(ip)
            ip = labels[arg if not arg.startswith('.') else scope + arg]
        elif cmd == 'sound_jump':
            ip = labels[arg if not arg.startswith('.') else scope + arg]
        elif cmd == 'duty_cycle_pattern':
            duty = int(arg.split(',')[0]) & 3
        elif cmd == 'duty_cycle':
            duty = int(arg) & 3
        elif cmd == 'pitch_offset':
            override = int(arg)
        elif cmd == 'pitch_sweep':
            pass                       # no hardware sweep in gbsynth
        elif cmd in ('square_note', 'noise_note'):
            length, vol, d, per, freq = parse_event(line, name)
            if cmd == 'noise_note':
                freq = noise_period(freq)
            out.append((freq, duty, vol, d, per, length + 1))
        else:
            raise SystemExit('unhandled command in %s: %s' % (name, line))
    return out, override


def gen2():
    """Johto: -> (bases, mons). bases[i] = (chans[3], overrides[2]); mons[dex] = (i, pitch, tempo)."""
    text = fetch('audio/cries.asm', RAW2)
    lines = []          # (global scope label, stripped line)
    labels = {}
    scope = ''
    for raw in text.splitlines():
        ln = raw.split(';')[0].strip()
        if ln.endswith(':') and not ln.startswith('.'):
            scope = ln[:-1]
            labels[scope] = len(lines)
        elif ln.endswith(':'):
            labels[scope + ln[:-1]] = len(lines)
        lines.append((scope, ln))
    # cry headers: which channel labels each cry uses
    headers = {}
    cur = None
    for sc, ln in lines:
        if re.fullmatch(r'Cry_\w+:', ln):
            cur = ln[:-1]
            headers[cur] = {}
        elif cur and ln.startswith('channel '):
            m = re.match(r'channel (\d+), (\S+)', ln)
            headers[cur][int(m.group(1))] = m.group(2)
        elif ln and not ln.startswith('channel') and not ln.startswith('Cry_'):
            cur = None if not ln.startswith('channel_count') else cur
    # CRY_* constant order == Cries pointer order
    consts = re.findall(r'const (CRY_\w+)', fetch('constants/cry_constants.asm', RAW2))
    ptrs = re.findall(r'dba (Cry_\w+)', fetch('audio/cry_pointers.asm', RAW2))
    if len(consts) != len(ptrs):
        raise SystemExit('cry constants (%d) and pointers (%d) disagree' % (len(consts), len(ptrs)))
    cry_label = dict(zip(consts, ptrs))
    cry_index = {c: i for i, c in enumerate(consts)}
    rows = re.findall(r'mon_cry (CRY_\w+),\s*(-?\d+),\s*(\d+)',
                      fetch('data/pokemon/cries.asm', RAW2))[:JOHTO_END]
    if len(rows) != JOHTO_END:
        raise SystemExit('expected %d PokemonCries rows, got %d' % (JOHTO_END, len(rows)))
    bases, base_of, mons = [], {}, {}
    for dex, (const, pitch, tempo) in enumerate(rows, 1):
        if dex <= KANTO:
            continue                  # Gen 1 stays on pokered's cries
        if const not in base_of:
            lab = cry_label[const]
            chans = [[], [], []]
            ov = [-1, -1]
            for ch, slot in ((5, 0), (6, 1), (8, 2)):
                if ch in headers[lab]:
                    ev, o = run_channel(lines, labels, labels[headers[lab][ch]], headers[lab][ch])
                    chans[slot] = ev
                    if slot < 2:
                        ov[slot] = o
            base_of[const] = len(bases)
            bases.append((chans, ov))
        mons[dex] = (base_of[const], int(pitch), int(tempo))
    return bases, mons


def main():
    # internal id (1-based) -> dex number
    consts = fetch('constants/pokedex_constants.asm')
    dexnum = {}
    for name, n in re.findall(r'const (DEX_\w+)\s*;\s*(\d+)', consts):
        dexnum[name] = int(n)
    order = re.findall(r'db (DEX_\w+|0)\b', fetch('data/pokemon/dex_order.asm'))
    crydata = fetch('data/pokemon/cries.asm').split('CryData::')[1]
    rows = re.findall(r'mon_cry SFX_CRY_([0-9A-F]+), \$([0-9A-F]+), \$([0-9A-F]+)',
                      crydata)
    if len(order) != len(rows):
        raise SystemExit('dex_order (%d) and CryData (%d) disagree'
                         % (len(order), len(rows)))

    table = {}          # dex -> (base, pitch, length)
    for dexname, (base, pitch, length) in zip(order, rows):
        if dexname == '0':
            continue
        table[dexnum[dexname]] = (int(base, 16), int(pitch, 16), int(length, 16))
    missing = [d for d in range(1, KANTO + 1) if d not in table]
    if missing:
        raise SystemExit('no cry for dex %s' % missing)

    bases = sorted({b for b, _, _ in table.values()})
    seqs = {}
    for b in bases:
        txt = fetch('audio/sfx/cry%02x_1.asm' % b)
        chans = []
        for ch in (5, 6, 8):
            ev = parse_channel(txt, 'SFX_Cry%02X_1_Ch%d' % (b, ch))
            chans.append(ev or [])
        seqs[b] = chans
        sys.stderr.write('cry %02x: %s events\n' % (b, [len(c) for c in chans]))

    g2bases, g2mons = gen2()
    g2ofs = max(bases) + 1               # Gen 2 bases are appended after Gen 1's
    sys.stderr.write('gen2: %d base cries for %d species\n' % (len(g2bases), len(g2mons)))

    L = ['// GENERATED by tools/gen_cries.py from pret/pokered and pret/pokecrystal - do not edit',
         '#pragma once', '#include <stdint.h>', '',
         '// Real Gen 1 and Gen 2 cries, as note events for gbsynth.cpp.',
         '// Nintendo / Game Freak / Creatures -- see CREDITS.md.', '',
         'struct CryNote {',
         '  uint16_t freq;     // pulse: Game Boy frequency register; noise: samples per LFSR clock',
         '  uint8_t duty;',
         '  uint8_t vol;',
         '  int8_t envDir;',
         '  uint8_t envPeriod;',
         '  uint8_t frames;    // length in 1/59.73 s, before the species length modifier',
         '};', '']
    names = {}
    for b in bases:
        for i, ch in enumerate(seqs[b]):
            n = 'CRY_%02X_%s' % (b, 'PQN'[i])
            names[(b, i)] = n
            if ch:
                L.append('static const CryNote %s[%d] = {' % (n, len(ch)))
                for e in ch:
                    L.append('  { %d, %d, %d, %d, %d, %d },' % e)
                L.append('};')
            L.append('')
    for k, (chans, ov) in enumerate(g2bases):
        for i, ch in enumerate(chans):
            n = 'CRY_G2_%02d_%s' % (k, 'PQN'[i])
            names[(g2ofs + k, i)] = n
            if ch:
                L.append('static const CryNote %s[%d] = {' % (n, len(ch)))
                for e in ch:
                    L.append('  { %d, %d, %d, %d, %d, %d },' % e)
                L.append('};')
            L.append('')
    L.append('struct CryBase {')
    L.append('  const CryNote *p1; uint8_t n1;')
    L.append('  const CryNote *p2; uint8_t n2;')
    L.append('  const CryNote *nz; uint8_t nn;')
    L.append('  int16_t set1, set2;   // -1, or a pitch that REPLACES the species pitch (pitch_offset)')
    L.append('};')
    L.append('#define CRY_BASES %d' % (g2ofs + len(g2bases)))
    L.append('static const CryBase CRY_BASE_TBL[CRY_BASES] = {')
    for b in range(g2ofs + len(g2bases)):
        if b < g2ofs and b not in seqs:
            L.append('  { nullptr, 0, nullptr, 0, nullptr, 0, -1, -1 },')
            continue
        parts = []
        chs = seqs[b] if b < g2ofs else g2bases[b - g2ofs][0]
        ov = [-1, -1] if b < g2ofs else g2bases[b - g2ofs][1]
        for i in range(3):
            n = names[(b, i)]
            c = len(chs[i])
            parts.append('%s, %d' % (n if c else 'nullptr', c))
        L.append('  { %s, %d, %d },' % (', '.join(parts), ov[0], ov[1]))
    L.append('};')
    L.append('')
    L.append('// pitch is signed (Crystal stores it that way); tempo is the pulse note-length scale,')
    L.append('// 256 = as written. Gen 1 rows hold length + 0x80 so one formula serves both.')
    L.append('struct CryMon { uint16_t base; int16_t pitch; uint16_t tempo; };')
    L.append('#define CRY_DEX %d' % JOHTO_END)
    L.append('// indexed by dex - 1')
    L.append('static const CryMon CRY_MON[CRY_DEX] = {')
    for d in range(1, JOHTO_END + 1):
        if d <= KANTO:
            b, p, l = table[d]
            L.append('  { %d, %d, %d },' % (b, p, l + 0x80))
        else:
            b, p, t = g2mons[d]
            L.append('  { %d, %d, %d },' % (g2ofs + b, p, t))
    L.append('};')
    L.append('')
    open(OUT, 'w').write('\n'.join(L))
    sys.stderr.write('wrote %s (%d species, %d+%d base cries)\n'
                     % (os.path.normpath(OUT), JOHTO_END, len(bases), len(g2bases)))


if __name__ == '__main__':
    main()
