"""Replace FUN_xxxxxxxx in REA/Ghidra pseudocode with names from names.json.
usage: python -I rename.py < pseudocode.txt     (or: python -I rename.py 0x561B10)"""
import json, re, sys, pathlib
names = json.load(open(pathlib.Path(__file__).with_name('names.json')))
by_addr = {int(k, 16): v['name'] for k, v in names.items()}
if len(sys.argv) > 1:
    for a in sys.argv[1:]:
        print(a, by_addr.get(int(a, 16), '?'))
    sys.exit()
def sub(m):
    n = by_addr.get(int(m.group(1), 16))
    return n.replace('::', '__') if n else m.group(0)
sys.stdout.write(re.sub(r'FUN_([0-9a-fA-F]{8})', sub, sys.stdin.read()))
