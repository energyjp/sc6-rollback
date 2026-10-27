#!/usr/bin/env python3
"""Static checks of SoulCalibur VI's EvaluateIf argument-loop bug and of the plugin's four-byte fix (docs/NOTES.md, Part 1).
READ-ONLY on SoulcaliburVI.exe. Needs pefile and capstone (pip install pefile capstone).

  python tools/argc_static.py ["<path to SoulcaliburVI.exe>"]

The default path is the standard Steam library location. The last line is ARGC_STATIC PASS|FAIL (n failures).

AS1  the four fix sites (src/argcfix.c k_argc_sites: VA, the 6 stock bytes, the fixed bytes) equal the exe's bytes, only the second opcode byte
     changes (the jcc condition; the rel32 displacement stays), and none of their bytes is a DIR64 relocation target. Teeth: a planted wrong
     stock byte is caught.
AS2  EvaluateIf (0x1403732f0) dispatches its sub-op through the image-relative jump table at 0x140376568; its entries for 0x24 / 0x25 / 0x90 /
     0x91 are the four case entries the plugin names.
AS3  all four sites are on ONE 4 KB page (0x140373000) of .text, Characteristics 0x60000020 (code, execute, read, NOT write), so the plugin needs
     VirtualProtect + FlushInstructionCache.
AS4  the fix makes 0x25 read like 0x24 and 0x91 read like 0x90: stock, 0x25 / 0x91 enter with js and loop with jle (args[1..argc], one past the
     arguments); patched (the byte change applied to a copy), they enter with jle and loop with jl, as 0x24 / 0x90 do (args[1..argc-1]).
AS5  the stock loop bounds of the four argument-loop cases: 0x24 / 0x90 exclusive (jl), 0x25 / 0x91 inclusive (jle). (This checks these four
     cases only; it is not a census of every EvaluateIf sub-op.)"""
import hashlib, os, re, sys, struct
import pefile, capstone

DEFAULT_EXE = r"C:\Program Files (x86)\Steam\steamapps\common\SoulcaliburVI\SoulcaliburVI\Binaries\Win64\SoulcaliburVI.exe"
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')
FAILS = []


def ok(cond, what, detail=''):
    print('%-4s %s%s' % ('ok' if cond else 'FAIL', what, (' :: ' + detail) if detail else ''))
    if not cond:
        FAILS.append(what)
    return cond


class Exe:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        self.pe = pefile.PE(data=self.data, fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.rel = set()
        for b in getattr(self.pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in b.entries:
                if e.type == 10:
                    self.rel.add(e.rva + self.base)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    def rd(self, va, n):
        o = self.pe.get_offset_from_rva(va - self.base)
        return self.data[o:o + n]

    def sec(self, va):
        for s in self.pe.sections:
            lo = self.base + s.VirtualAddress
            if lo <= va < lo + max(s.Misc_VirtualSize, s.SizeOfRawData):
                return s.Name.rstrip(b'\0').decode(), s.Characteristics
        return '?', 0


def parse_sites():
    """src/argcfix.c k_argc_sites: {VA, {stock 6}, {fixed 6}, "what"}"""
    t = open(os.path.join(SRC, 'argcfix.c'), encoding='utf-8').read()
    m = re.search(r'k_argc_sites\[ARGC_NSITES\]\s*=\s*\{(.*?)\n\};', t, re.S)
    rows = re.findall(r'\{\s*(0x[0-9a-fA-F]+)ULL,\s*\{([^}]*)\},\s*\{([^}]*)\},\s*"([^"]*)"\s*\}', m.group(1))
    out = []
    for va, s, f, what in rows:
        sb = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', s)]
        fb = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', f)]
        out.append((int(va, 16), bytes(sb), bytes(fb), what))
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    try:
        exe = Exe(path)
    except FileNotFoundError:
        print('ARGC_STATIC SKIP (exe not found: %s; pass its path as the first argument)' % path)
        return 2
    print('exe sha256', hashlib.sha256(exe.data).hexdigest(), 'ts', hex(exe.pe.FILE_HEADER.TimeDateStamp))
    sites = parse_sites()

    # AS1 the sites
    allok, relhit = True, []
    for va, sb, fb, what in sites:
        got = exe.rd(va, 6)
        m = got == sb
        changed = [i for i in range(6) if sb[i] != fb[i]]
        allok &= m and changed == [1]
        rels = [va + i for i in range(6) if (va + i) in exe.rel]
        relhit += rels
        print('   site %x %-24s exe=%s stock=%s %s fixed_byte=+%d %s relocs=%s' % (
            va, what, got.hex(), sb.hex(), 'OK' if m else 'MISMATCH', changed[0] if len(changed) == 1 else -1, fb.hex(), [hex(r) for r in rels]))
    ok(allok and not relhit, 'AS1 the four sites equal the exe, only the second opcode byte changes, no byte is a DIR64 relocation')
    bad = (sites[0][0], bytes([sites[0][1][0] ^ 1]) + sites[0][1][1:])
    ok(exe.rd(bad[0], 6) != bad[1], 'AS1 teeth: a planted wrong stock byte would be caught (exe != the altered row)')

    # AS2 the dispatch
    want = {0x24: 0x140373909, 0x25: 0x1403739bc, 0x90: 0x140373a6e, 0x91: 0x140373b2d}
    jt = 0x140376568
    tgtok = True
    for sub, tva in want.items():
        e = struct.unpack_from('<I', exe.rd(jt + sub * 4, 4))[0] + exe.base
        tgtok &= e == tva
        print('   sub 0x%02x -> %x (want %x) %s' % (sub, e, tva, 'OK' if e == tva else 'BAD'))
    ok(tgtok, 'AS2 the jump table at 0x140376568 sends 0x24/0x25/0x90/0x91 to the four case entries')

    # AS3 one non-writable .text page
    pages = sorted(set((va & ~0xfff) for va, _, _, _ in sites))
    name, ch = exe.sec(sites[0][0])
    onepage = len(pages) == 1 and pages[0] == 0x140373000
    ok(onepage and name == '.text' and ch == 0x60000020, 'AS3 all four sites on one .text page 0x140373000, Characteristics 0x60000020 (not writable)',
       'pages=%s sec=%s ch=0x%x' % ([hex(p) for p in pages], name, ch))

    # AS4 patched 0x25 / 0x91 read like 0x24 / 0x90
    def first_jccs(code, entry):
        d = list(exe.md.disasm(code, entry))
        ej = next((i for i in d if i.mnemonic in ('js', 'jle', 'je') and i.address < entry + 0x12), None)
        lj = next((i for i in d if i.mnemonic in ('jl', 'jle') and i.op_str.startswith('0x') and int(i.op_str, 16) < i.address), None)
        return ej, lj

    def case_jccs(entry, patch=()):
        buf = bytearray(exe.rd(entry, 0x120))
        for va, s, f, _ in [sites[i] for i in patch]:
            buf[(va + 1) - entry] = f[1]
        return first_jccs(bytes(buf), entry)
    e24, l24 = case_jccs(0x140373909)
    e90, l90 = case_jccs(0x140373a6e)
    e25s, l25s = case_jccs(0x1403739bc)
    e91s, l91s = case_jccs(0x140373b2d)
    e25, l25 = case_jccs(0x1403739bc, (0, 1))
    e91, l91 = case_jccs(0x140373b2d, (2, 3))
    print('   sub 0x24 entry=%s loop=%s' % (e24.mnemonic, l24.mnemonic))
    print('   sub 0x25 stock entry=%s loop=%s  patched entry=%s loop=%s' % (e25s.mnemonic, l25s.mnemonic, e25.mnemonic, l25.mnemonic))
    print('   sub 0x90 entry=%s loop=%s' % (e90.mnemonic, l90.mnemonic))
    print('   sub 0x91 stock entry=%s loop=%s  patched entry=%s loop=%s' % (e91s.mnemonic, l91s.mnemonic, e91.mnemonic, l91.mnemonic))
    r = (e25s.mnemonic == 'js' and l25s.mnemonic == 'jle' and e25.mnemonic == 'jle' and l25.mnemonic == 'jl' and
         e91s.mnemonic == 'js' and l91s.mnemonic == 'jle' and e91.mnemonic == 'jle' and l91.mnemonic == 'jl' and
         e24.mnemonic == 'jle' and l24.mnemonic == 'jl' and e90.mnemonic == 'jle' and l90.mnemonic == 'jl')
    ok(r, 'AS4 patched 0x25 = 0x24 and patched 0x91 = 0x90 (entry jcc jle, loop jcc jl); stock 0x25/0x91 are js/jle (inclusive)')

    # AS5 the stock loop bounds of the four cases
    bound = {0x24: l24.mnemonic, 0x25: l25s.mnemonic, 0x90: l90.mnemonic, 0x91: l91s.mnemonic}
    print('   stock loop bound: ' + ', '.join('0x%02x=%s' % (k, 'exclusive(jl)' if v == 'jl' else 'inclusive(%s)' % v) for k, v in sorted(bound.items())))
    ok(bound[0x24] == 'jl' and bound[0x90] == 'jl' and bound[0x25] == 'jle' and bound[0x91] == 'jle',
       'AS5 0x24/0x90 are exclusive and 0x25/0x91 inclusive (one argument past argc)')

    print('ARGC_STATIC %s (%d failures)' % ('PASS' if not FAILS else 'FAIL', len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
