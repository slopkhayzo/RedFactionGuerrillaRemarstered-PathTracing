"""Generate src/rfgrt/game/rfg_symbols.generated.h from rfg.pdb and the Steam build's map.

python tools/gen_symbols.py [--list tools/symbols.txt] [--map tools/steam_map.json]
                            [--out src/rfgrt/game/rfg_symbols.generated.h]

Adapted from the PhysX project's tools/gen_symbols.py, with struct layouts added: besides function and global addresses,
the header carries the offset of every struct member and the size of every struct the plugin reads (`field` and
`size` entries), per build, so the plugin never hard-codes a layout. Today's builds (GOG and Steam) are both 32-bit and
share their layouts; a 64-bit rfg.exe (the 64-bit port) will add a build with its own addresses and offsets, read from
its own PDB.

The PDB is read with the PhysX project's analysis modules (its research/scripts; set RFGRE_SCRIPTS to use another
copy). Their tables (procs.tsv, data.tsv, publics.tsv) are created with pdbdump.py when missing. The Steam build ships no
PDB: the PhysX project's tools/match_builds.py found each GOG function and global in its code, and tools/steam_map.json
(copied from there) gives every address here a Steam address. An address it cannot map stops the generation.
"""
import argparse
import bisect
import json
import os
import re
import struct
import subprocess
import sys
import uuid

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SCRIPTS = os.environ.get('RFGRE_SCRIPTS') or os.path.normpath(
    os.path.join(ROOT, '..', 'RedFactionGuerrillaRemarstered-PhysX', 'research', 'scripts'))
if not os.path.exists(os.path.join(SCRIPTS, 'rfgre_paths.py')):
    sys.exit('PDB analysis scripts not found in %s (set RFGRE_SCRIPTS)' % SCRIPTS)
sys.path.insert(0, SCRIPTS)
import rfgre_paths as P  # noqa: E402

if not os.path.exists(os.path.join(P.OUT, 'procs.tsv')):
    subprocess.check_call([sys.executable, os.path.join(SCRIPTS, 'pdbdump.py')])

import rdis  # noqa: E402
import sig as sigmod  # noqa: E402  (prints nothing when imported without arguments)
import tpi  # noqa: E402
from pdbmsf import PDB  # noqa: E402


def ident(name):
    return re.sub(r'[^0-9A-Za-z_]', '_', name.replace('::', '__'))


def proc_signatures():
    sigs = {}
    for line in open(os.path.join(P.OUT, 'procs.tsv')):
        a, sz, g, ft, name, mod = line.rstrip('\n').split('\t')
        try:
            sigs[int(a, 16)] = sigmod.sig(int(ft, 16))
        except Exception:
            sigs[int(a, 16)] = '?'
    return sigs


def data_types():
    types = {}
    for line in open(os.path.join(P.OUT, 'data.tsv')):
        a, t, k, name, mod = line.rstrip('\n').split('\t')
        types.setdefault((name, int(a, 16)), int(t, 16))
    return types


def build_identity():
    pe_ts = struct.unpack_from('<I', rdis.img, rdis.pe.DOS_HEADER.e_lfanew + 8)[0]
    size_of_image = rdis.pe.OPTIONAL_HEADER.SizeOfImage
    info = PDB(P.PDB).msf.read(1)
    ver, sig, age = struct.unpack_from('<III', info, 0)
    guid = uuid.UUID(bytes_le=bytes(info[12:28]))
    return pe_ts, size_of_image, guid, age


class BuildMap:
    """GOG RVA -> the other build's, from the PhysX project's tools/match_builds.py ranges."""

    def __init__(self, path):
        m = json.load(open(path))
        self.gog, self.other = m['gog'], m['other']
        self.text = sorted(m['text'])
        self.data = sorted(m['data'])
        self.unmatched = sorted(m['unmatched'])
        self._t = [t[0] for t in self.text]
        self._d = [d[0] for d in self.data]
        self._u = [u[0] for u in self.unmatched]

    def code(self, a):
        k = bisect.bisect_right(self._u, a) - 1
        if k >= 0 and self.unmatched[k][0] <= a <= self.unmatched[k][1]:
            return None, 'differs in that build (%s)' % self.unmatched[k][2]
        k = bisect.bisect_right(self._t, a) - 1
        if k >= 0 and self.text[k][0] <= a <= self.text[k][1]:
            return a + self.text[k][2], ''
        return None, 'outside the matched code'

    def data_(self, a):
        k = bisect.bisect_right(self._d, a) - 1
        if k >= 0 and self.data[k][0] <= a <= self.data[k][1]:
            return a + self.data[k][2], ''
        prev = self.data[k] if k >= 0 else None
        nxt = self.data[k + 1] if k + 1 < len(self.data) else None
        if prev and nxt and prev[2] == nxt[2]:
            return a + prev[2], ''
        return None, 'between data ranges with different shifts (%s / %s)' % (prev, nxt)


def member_offset(type_name, member):
    """Offset of a (possibly inherited) member: searches the type, then its base classes."""
    r = tpi.layout(type_name)
    if not r:
        sys.exit('type %s not found' % type_name)
    ti, size, fl = r
    for kind, off, name, typ in fl:
        if kind == 'member' and name == member:
            return off, tpi.tname(typ)
    for kind, off, name, typ in fl:
        if kind == 'base':
            try:
                o, t = member_offset(name, member)
                return off + o, t
            except SystemExit:
                pass
    sys.exit('%s has no member %s' % (type_name, member))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', default=os.path.join(ROOT, 'tools', 'symbols.txt'))
    ap.add_argument('--map', default=os.path.join(ROOT, 'tools', 'steam_map.json'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'src', 'rfgrt', 'game', 'rfg_symbols.generated.h'))
    args = ap.parse_args()

    sigs = proc_signatures()
    dtypes = data_types()
    pe_ts, soi, guid, age = build_identity()
    if str(guid) != P.EXPECTED_PDB_GUID or age != P.EXPECTED_PDB_AGE:
        sys.exit('PDB %s age %d is not the documented build' % (guid, age))
    steam = BuildMap(args.map)
    if (steam.gog['pdb_guid'] != guid.bytes_le.hex() or steam.gog['pdb_age'] != age or
            steam.gog['timestamp'] != pe_ts or steam.gog['size_of_image'] != soi):
        sys.exit('%s was made from another GOG build: run the PhysX project\'s tools/match_builds.py again' % args.map)

    # (C++ name, GOG RVA, Steam RVA, comment); functions also have a size. Offsets: (C++ name, x86 value, comment).
    fns, vars_, offs, enums, errors = [], [], [], [], []
    for raw in open(args.list):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        kind, rest = line.split(None, 1)
        rest, _, alias = rest.partition(' as ')
        alias = alias.strip()
        m = re.match(r'^(.*?)\s+@([0-9A-Fa-f]{8})$', rest.strip()) if kind == 'fn' else None
        name, at = (m.group(1).strip(), m.group(2)) if m else (rest.strip(), '')
        if kind == 'fn':
            cands = sorted(a for a in set(rdis.byname.get(name, [])) if a in rdis.procs)
            if at:
                want = int(at, 16)
                if want not in cands:
                    sys.exit('%s has no function at %s (candidates %s)' % (name, at, [hex(c) for c in cands]))
                cands = [want]
            if len(cands) != 1:
                sys.exit('%s: expected one function, found %s' % (name, [hex(c) for c in cands]))
            a = cands[0]
            s, why = steam.code(a)
            if s is None:
                errors.append('fn %s (%08X): %s' % (name, a, why))
            size = rdis.procsizes[a]
            fns.append((alias or ident(name), a, s, '%s, %d bytes: %s' % (name, size, sigs.get(a, '?')), size))
        elif kind == 'var':
            cands = sorted({a for (n, a) in dtypes if n == name})
            if len(cands) != 1:
                sys.exit('%s: expected one global, found %s' % (name, [hex(c) for c in cands]))
            a = cands[0]
            s, why = steam.data_(a)
            if s is None:
                errors.append('var %s (%08X): %s' % (name, a, why))
            vars_.append((alias or ident(name), a, s, tpi.tname(dtypes[(name, a)])))
        elif kind == 'field':
            type_name, _, member = name.rpartition('::')
            off, tname = member_offset(type_name, member)
            offs.append((alias or ident(name), off, '%s::%s: %s' % (type_name, member, tname)))
        elif kind == 'size':
            r = tpi.layout(name)
            if not r:
                sys.exit('type %s not found' % name)
            offs.append((alias or ident(name) + '__size', r[1], 'sizeof(%s)' % name))
        elif kind == 'enum':
            r = tpi.layout(name)
            if not r:
                sys.exit('enum %s not found' % name)
            enums.append((name, [(v, n) for k, v, n, t in r[2] if k == 'enum']))
        else:
            sys.exit('unknown entry: ' + raw)
    if errors:
        sys.exit('no Steam address for:\n  ' + '\n  '.join(errors))

    def guid_bytes(hexstr):
        return ', '.join('0x%02X' % b for b in bytes.fromhex(hexstr))

    o = steam.other
    out = []
    out.append('// Generated by tools/gen_symbols.py from rfg.pdb (GOG build) and tools/steam_map.json (Steam build).')
    out.append('// Do not edit; edit tools/symbols.txt instead.')
    out.append('#pragma once')
    out.append('#include <cstdint>')
    out.append('')
    out.append('namespace rfg {')
    out.append('')
    out.append('// The builds of rfg.exe these addresses cover, told apart by the PDB GUID and age in their debug directory.')
    out.append('// `machine` is the PE machine type: a plugin only takes builds of its own architecture.')
    out.append('struct BuildId {')
    out.append('    const char* name;')
    out.append('    uint32_t exe_timestamp, exe_size_of_image;')
    out.append('    uint8_t pdb_guid[16];')
    out.append('    uint32_t pdb_age;')
    out.append('    uint16_t machine;')
    out.append('};')
    out.append('inline constexpr int kBuildCount = 2;')
    out.append('inline constexpr BuildId kBuilds[kBuildCount] = {')
    out.append('    {"GOG", 0x%08X, 0x%08X, {%s}, %d, 0x014C},' % (pe_ts, soi, guid_bytes(guid.bytes_le.hex()), age))
    out.append('    {"Steam", 0x%08X, 0x%08X, {%s}, %d, 0x014C},' % (o['timestamp'], o['size_of_image'],
                                                                    guid_bytes(o['pdb_guid']), o['pdb_age']))
    out.append('};')
    out.append('')
    out.append('// RVAs in the running build: the GOG build\'s until select_build() sets another build\'s (rfg.h).')
    out.append('namespace rva {')
    for cname, a, s, comment, size in fns:
        out.append('inline uint32_t %s = 0x%08X;  // Steam 0x%08X; %s' % (cname, a, s, comment))
    out.append('')
    for cname, a, s, t in vars_:
        out.append('inline uint32_t %s = 0x%08X;  // Steam 0x%08X; %s' % (cname, a, s, t))
    out.append('}  // namespace rva')
    out.append('')
    out.append('// Struct member offsets and struct sizes in the running build (the 32-bit builds share theirs).')
    out.append('namespace off {')
    for cname, v, comment in offs:
        out.append('inline uint32_t %s = 0x%X;  // %s' % (cname, v, comment))
    out.append('}  // namespace off')
    out.append('')
    out.append('// Every value above in each build, in the order of kBuilds.')
    out.append('struct RvaEntry {')
    out.append('    uint32_t* value;')
    out.append('    uint32_t in_build[kBuildCount];')
    out.append('};')
    out.append('inline constexpr RvaEntry kRvaTable[] = {')
    for cname, a, s, *_ in fns + vars_:
        out.append('    {&rva::%s, {0x%08X, 0x%08X}},' % (cname, a, s))
    for cname, v, _ in offs:
        out.append('    {&off::%s, {0x%X, 0x%X}},' % (cname, v, v))
    out.append('};')
    out.append('')
    out.append('// Code size of each function above (call-site range checks: return address - rva < size).')
    out.append('namespace size {')
    for cname, a, s, comment, size in fns:
        out.append('inline constexpr uint32_t %s = %d;' % (cname, size))
    out.append('}  // namespace size')
    for name, items in enums:
        out.append('')
        out.append('struct %s_name { int value; const char* name; };' % ident(name))
        out.append('inline constexpr %s_name %s_names[] = {' % (ident(name), ident(name)))
        for v, n in items:
            out.append('    {%d, "%s"},' % (v, n))
        out.append('};')
    out.append('')
    out.append('}  // namespace rfg')
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote %s: %d functions, %d globals, %d offsets and sizes, %d enums, Steam addresses for all of them'
          % (args.out, len(fns), len(vars_), len(offs), len(enums)))


if __name__ == '__main__':
    main()
