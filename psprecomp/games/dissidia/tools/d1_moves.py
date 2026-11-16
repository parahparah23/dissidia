#!/usr/bin/env python3
"""Bartz's Dissidia (2008) moves for Dissidia 012: build mods/bartz-dff from your two discs.

    d1_moves.py --d1 "Dissidia Final Fantasy (USA).iso" --iso012 "Dissidia 012 (USA).iso" [--out mods/bartz-dff]

012 dropped eight of the moves Bartz had in the first Dissidia: Ascendant Lance, Climbarrel (ground Bravery), Storm Shot,
Slidehazzard (air Bravery), Soul Eater, Flood (ground HP), Wind Shear and Paladin Force (air HP). This puts them back as
new abilities next to his 012 kit (he keeps everything he has). The result is a "kind": "retail" mod folder, built into
the game by run.sh / charmod.py like any other; Settings > Mods switches the moves on and off (retail_moves.txt).

Steps:
  1. both packages are read straight from the ISOs (PACKAGE.BIN by path CRC);
  2. D1's p_fiv100.objx is rewritten in the 012 layout (d1_objx.py: actions, commands, hits, projectiles, effect
     placements, event ops) and unpacked next to D1's model as a mod folder "d1bartz" (based_on fiv100);
  3. retail Bartz is unpacked as the folder "bartz";
  4. the Move Studio copies the eight commands from d1bartz into bartz in a headless Chrome (d1_moves_studio.js: the
     Studio's own copy-a-move, so animations go into all four costumes, hits, hitboxes, projectiles, effects, sounds,
     voices and face textures come along);
  5. names, help texts, CP / AP and learn levels are set, and the battle voice bank gets D1's lines for the slots 012
     left empty (the banks are slot-for-slot the same; only the English bank: D1 has no Japanese voices).

Needs Python 3, Node.js with playwright-core (npm i playwright-core; NODE_PATH or a node_modules next to this file) and
Chrome / Chromium (CHROME=<binary>, default /usr/bin/google-chrome). Only tested with the USA discs of both games.
The folder holds data from both discs: keep it to yourself (mods/ is not in git).
"""
import argparse
import glob
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import charmod_gmo as G  # noqa: E402
import charmod_objx as co  # noqa: E402
import d1_objx  # noqa: E402
import package_extract as pe  # noqa: E402
import roster_mainbin as rm  # noqa: E402
import voice_bank as vb  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
D1_VOICE_BANK = 0xDD2E4D13      # D1 has no package path list for it: Bartz's battle bank (SSCF bank id 18100), by CRC
VOICE_012 = 'voice/en/battle/voice_p_fiv100_00.scd'
VOICE_BASE = 1810000            # voice event ids: bank id * 100 + slot

# D1 command -> (name, type, phase, help, CP, CP mastered, AP, learn level). CP / AP from D1's own ability records;
# learn levels: D1's for Climbarrel 3, Flood 19, Slidehazzard 28, Paladin Force 38, chosen for the rest.
MOVES = [
    (0x198, 'Ascendant Lance', 'brave', 'ground', "[Ground] A lance lunge. Press\nagain for a flurry that ends in a\nblow. (Dissidia 2008)", 35, 15, 120, 6),
    (0x19A, 'Climbarrel', 'brave', 'ground', "[Ground] Rising strikes into a\ncombo that blows the foe away.\n(Dissidia 2008)", 35, 20, 120, 3),
    (0x19E, 'Storm Shot', 'brave', 'midair', "[Midair] A gust of strikes. Press\nagain to blow the foe away.\n(Dissidia 2008)", 35, 15, 180, 12),
    (0x19F, 'Slidehazzard', 'brave', 'midair', "[Midair] A four-part combo that\ncarries the foe down to the ground.\n(Dissidia 2008)", 35, 15, 180, 28),
    (0x19C, 'Soul Eater', 'hp', 'ground', "[Ground] HP attack. A strike, then\ndark slashes and the HP blow.\n(Dissidia 2008)", 45, 25, 180, 24),
    (0x19D, 'Flood', 'hp', 'ground', "[Ground] HP attack. A torrent of\nwater sweeps the foe away.\n(Dissidia 2008)", 45, 25, 180, 19),
    (0x1A1, 'Wind Shear', 'hp', 'midair', "[Midair] HP attack. A whirlwind\nholds the foe, then blasts it.\n(Dissidia 2008)", 45, 25, 300, 46),
    (0x1A2, 'Paladin Force', 'hp', 'midair', "[Midair] HP attack. Holy blades\ncrash down on the foe.\n(Dissidia 2008)", 45, 25, 180, 38),
]


class IsoPackage(pe.Package):
    """PACKAGE.BIN / PACKAGE_INFO.BIN read in place from an ISO 9660 image."""

    def __init__(self, iso):
        self.iso = open(iso, 'rb')
        info_lba, info_size = self._find('PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN')
        self.pkg_lba, _ = self._find('PSP_GAME/USRDIR/DATA/PACKAGE.BIN')
        self.iso.seek(info_lba * 2048)
        idx = self.iso.read(info_size)
        if idx[4:8] != b'pack':
            raise SystemExit('%s: PACKAGE_INFO.BIN is not a Dissidia package index' % iso)
        n = struct.unpack_from('<I', idx, 8)[0]
        self.entries = {}
        for i in range(n):
            crc, sector, sf = struct.unpack_from('<III', idx, 16 + 12 * i)
            self.entries[crc] = (sector, sf & 0x0FFFFFFF, sf >> 28)

    def _find(self, path):
        self.iso.seek(16 * 2048)
        pvd = self.iso.read(2048)
        if pvd[1:6] != b'CD001':
            raise SystemExit('not an ISO 9660 image')
        rec = pvd[156:190]
        for part in path.split('/'):
            lba, size = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
            self.iso.seek(lba * 2048)
            d = self.iso.read(size)
            i, found = 0, None
            while i < len(d):
                ln = d[i]
                if not ln:
                    i = (i // 2048 + 1) * 2048
                    continue
                r = d[i:i + ln]
                if r[33:33 + r[32]].decode('latin1').split(';')[0].upper() == part.upper():
                    found = r
                    break
                i += ln
            if found is None:
                raise SystemExit('%s not found in the ISO' % path)
            rec = found
        return struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]

    def read_crc(self, crc):
        sector, size, _ = self.entries[crc]
        self.iso.seek((self.pkg_lba + sector) * 2048)
        return self.iso.read(size)


def prepare(src, d1, d12):
    """<src>/d1bartz (D1, 012 layout) and <src>/bartz (retail Bartz) for the Studio."""
    os.makedirs(src + '/_template')
    with open(src + '/_template/mod.json', 'w') as f:
        f.write('{}')
    gmo = d1.read('obj/p_fiv100_1p.gmo')
    os.makedirs(src + '/d1bartz/models')
    with open(src + '/d1bartz/models/p_d1b_1p.gmo', 'wb') as f:
        f.write(gmo)
    co.unpack(d1_objx.convert(d1.read('obj/p_fiv100.objx')), src + '/d1bartz/moveset/p_d1b', bones=G.Model(gmo).bone_names())
    names = {'%02X' % (0x60 + i): {'cmd': '0x%X' % c, 'name': n, 'type': t, 'phase': ph}
             for i, (c, n, t, ph, *_r) in enumerate(MOVES)}
    with open(src + '/d1bartz/mod.json', 'w') as f:      # based_on = this character: voices and face textures come along
        json.dump({'character': {'name': 'Bartz (DFF)', 'code': 'd1b', 'based_on': 'fiv100'}, 'move_names': names, 'files': {}}, f, indent=1)
    os.makedirs(src + '/bartz/models')
    files = {}
    for ms, rig in (('p_fiv100', 'p_fiv100_1p'), ('p_fiv100_a', 'p_fiv100_1p_a')):
        g = d12.read('obj/%s.gmo' % rig)
        with open('%s/bartz/models/%s.gmo' % (src, rig), 'wb') as f:
            f.write(g)
        co.unpack(d12.read('obj/%s.objx' % ms), '%s/bartz/moveset/%s' % (src, ms), bones=G.Model(g).bone_names())
        files['obj/%s.objx' % ms] = 'moveset/' + ms
        files['obj/%s.gmo' % rig] = 'models/%s.gmo' % rig
    with open(src + '/bartz/mod.json', 'w') as f:      # move_names: the retail abilities, filled in by the page from the ISO
        json.dump({'kind': 'retail', 'character': {'name': 'Bartz', 'code': 'fiv100'}, 'move_names': {}, 'files': files}, f, indent=1)


def finish(mod, d1, d12):
    """Names, help, costs, learn levels; D1's voice lines in the slots 012's bank left empty."""
    with open(mod + '/mod.json') as f:
        m = json.load(f)
    by_name = {mv[1]: mv for mv in MOVES}
    for e in m['move_names'].values():
        if 'copy' not in e:
            continue
        cmd, name, _t, _p, help_, cp, cpm, ap, lv = by_name[e['name']]
        rm.expand_help(help_, '')
        e.update(help=help_, cp=cp, cp_mastered=cpm, ap=ap, learn=lv)
        e.pop('status', None)
        e['_note'] = 'Dissidia (2008) Bartz move, command 0x%X there (tools/d1_moves.py)' % cmd
    m['display'] = 'Bartz: Dissidia (2008) moves'
    ms = mod + '/moveset/p_fiv100'
    rj = lambda p: json.load(open(p))  # noqa: E731
    acts = {a['action']: a['script'] for a in rj(ms + '/actions.json')['actions']}
    rows = rj(ms + '/motion_map.json')['rows']
    new_cmds = {int(e['cmd'], 16) for e in m['move_names'].values() if 'copy' in e}
    motions = set()
    for c in rj(ms + '/moves.json')['commands']:
        if int(c['id'], 16) in new_cmds:
            for st in c['steps']:
                motions |= {r['motion'] for r in rows if r['script'] == acts.get(st['action'])}
    used = set()
    for p in glob.glob(ms + '/timelines/*.json'):
        t = rj(p)
        if not p.endswith('index.json') and t['motion'] in motions:
            used |= {e['voice'] - VOICE_BASE for e in t['ranged'] + t['keyed'] if e.get('op') == 'voice'}
    b12, b1 = vb.Bank.parse(d12.read(VOICE_012)), vb.Bank.parse(d1.read_crc(D1_VOICE_BANK))
    filled = []
    for i in sorted(used):
        if 0 <= i < min(len(b12.entries), len(b1.entries)) and b12.entries[i]['kind'] == 'empty' and b1.entries[i]['kind'] == 'data':
            b12.entries[i] = {'raw': bytearray(b1.entries[i]['raw']), 'kind': 'data', 'owner': None, 'blob': b1.entries[i]['blob']}
            filled.append(i)
    if filled:
        p = mod + '/files/' + VOICE_012
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(b12.to_bytes())
        m['files'][VOICE_012] = 'files/' + VOICE_012
    with open(mod + '/mod.json', 'w') as f:
        json.dump(m, f, indent=1, ensure_ascii=False)
        f.write('\n')
    print('voice lines from Dissidia (2008) in slots %s' % ', '.join(map(str, filled)))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--d1', required=True, help='Dissidia Final Fantasy (2008) ISO')
    ap.add_argument('--iso012', required=True, help='Dissidia 012 ISO (the one your game was set up from)')
    ap.add_argument('--out', default=os.path.join(REPO, 'mods', 'bartz-dff'))
    ap.add_argument('--keep', action='store_true', help='keep the scratch folder')
    a = ap.parse_args(argv)
    if os.path.exists(a.out):
        raise SystemExit('%s exists: delete it first' % a.out)
    d1, d12 = IsoPackage(a.d1), IsoPackage(a.iso012)
    if not d1.entries or rm.MAIN_CRC not in d12.entries:
        raise SystemExit('not the expected discs')
    tmp = tempfile.mkdtemp(prefix='d1moves_')
    try:
        src, out = tmp + '/src', tmp + '/out'
        prepare(src, d1, d12)
        js = os.path.join(HERE, 'd1_moves_studio.js')
        env = dict(os.environ)
        env.setdefault('NODE_PATH', os.path.join(HERE, 'node_modules'))
        r = subprocess.run(['node', js, os.path.join(REPO, 'studio', 'index.html'), src, os.path.abspath(a.iso012), out,
                            ','.join('%X' % mv[0] for mv in MOVES)], env=env, capture_output=True, text=True)
        log = r.stdout + r.stderr
        for line in log.splitlines():
            if 'TOAST' in line and 'retail characters' not in line or 'pageerror' in line:
                print(line.replace('[page] TOAST : ', '  '))
        if r.returncode or not os.path.exists(out + '/bartz/mod.json'):
            print(log[-3000:])
            raise SystemExit('the Move Studio step failed')
        finish(out + '/bartz', d1, d12)
        shutil.copytree(out + '/bartz', a.out)
        print('wrote %s: start the game (run.sh builds it in), or charmod.py build' % a.out)
    finally:
        if a.keep:
            print('scratch kept in', tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main(sys.argv[1:])
