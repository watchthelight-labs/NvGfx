# vtables.py <binary>: print every __ZTV vtable as "vtable slot target", targets resolved through external relocations
# (imported symbols) or the defined symbol at the stored address (local functions)
# <wtl> fork: nvgfx | author: watchthelight </wtl>
import subprocess, sys, re
b = sys.argv[1]
def run(*a): return subprocess.run(a, capture_output=True, text=True).stdout
syms, byaddr = [], {}
for l in run("nm", "-nm", b).splitlines():
    m = re.match(r"([0-9a-f]+) \((\w+),(\w+)\) .*? (\S+)$", l)
    if m:
        a = int(m.group(1), 16); syms.append((a, m.group(3), m.group(4))); byaddr.setdefault(a, m.group(4))
ext = {}
for l in run("otool", "-rv", b).splitlines():
    m = re.match(r"([0-9a-f]{8}) \S+ \S+\s+\S+\s+True\s+\S+\s+\S+\s+(\S+)", l)
    if m: ext[int(m.group(1), 16)] = m.group(2)
mem = {}
for sect in (("__DATA", "__const"), ("__DATA_CONST", "__const")):
    for l in run("otool", "-s", *sect, b).splitlines()[2:]:
        p = l.split("\t", 1)
        if len(p) < 2: continue
        base = int(p[0], 16); bs = bytes(int(x, 16) for x in p[1].split())
        for i, v in enumerate(bs): mem[base + i] = v
syms.sort()
for i, (a, sec, name) in enumerate(syms):
    if not name.startswith("__ZTV"): continue
    end = next((s for s, _, _ in syms[i + 1:] if s > a), a + 8)
    for off in range(0, end - a, 8):
        ad = a + off
        if ad in ext: t = ext[ad]
        else:
            v = int.from_bytes(bytes(mem.get(ad + k, 0) for k in range(8)), "little")
            t = "0" if v == 0 else byaddr.get(v, "local+0x%x" % v)
        print(name, off // 8, t)
