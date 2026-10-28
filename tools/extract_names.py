import json, re, sys, pathlib

src = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])

RE_CLASS = re.compile(r'RH_Scoped(?:Virtual)?Class\(\s*(\w+)')
RE_NS = re.compile(r'RH_ScopedNamespace(?:Name)?\(\s*"?([\w:]+)"?')
RE_NAMEDCLASS = re.compile(r'RH_ScopedNamedClass\(\s*(\w+)\s*,\s*"([^"]*)"')
RE_INST = re.compile(
    r'RH_Scoped(\w*Install|\w*Destructor\w*|\w*Constructor\w*)\(\s*(\w+)\s*,\s*(?:"[^"]*"\s*,\s*)?(0x[0-9A-Fa-f]+)')
RE_NAMED_INST = re.compile(
    r'RH_ScopedNamed(\w*)Install\(\s*(\w+)\s*,\s*"([^"]*)"\s*,\s*(0x[0-9A-Fa-f]+)')

names = {}
dups = 0
for f in sorted(src.rglob('*.cpp')):
    cls = None
    for line in f.read_text(errors='ignore').splitlines():
        s = line.strip()
        if s.startswith('//'):
            continue
        m = RE_NAMEDCLASS.search(s)
        if m:
            cls = m.group(2) or m.group(1)
            continue
        m = RE_CLASS.search(s)
        if m:
            cls = m.group(1)
            continue
        m = RE_NS.search(s)
        if m and 'Class' not in s:
            cls = m.group(1)
            continue
        m = RE_INST.search(s)
        if not m:
            continue
        kind, fn, addr = m.groups()
        a = int(addr, 16)
        if not (0x401000 <= a < 0x1556000):
            continue
        if 'Global' in kind or cls is None:
            full = fn
        else:
            full = f'{cls}::{fn}'
        if a in names:
            dups += 1
            continue
        names[a] = {'name': full, 'file': str(f.relative_to(src)).replace('\\', '/'), 'kind': kind}

out.write_text(json.dumps({f'0x{a:X}': v for a, v in sorted(names.items())}, indent=0))
print('unique addresses', len(names), 'dup-skipped', dups)
