#!/usr/bin/env python3
# compare_nvrm.py <shipped NVRM binary> <built NVRM binary> [report.txt]
# Static comparison of a rebuilt NVRM.kext binary with the shipped one: file size, load commands, section sizes,
# undefined (imported KPI) symbols, exported symbols, all symbol names, and per-function code size. Uses only nm and
# otool, never loads or runs anything.
import collections, subprocess, sys

def run(*a):
    return subprocess.run(a, capture_output=True, text=True, check=True).stdout

def sections(b):
    out, seg, sect = [], None, None
    for l in run("otool", "-l", b).splitlines():
        p = l.split()
        if not p: continue
        if p[0] == "sectname": sect = p[1]
        elif p[0] == "segname" and sect: seg = p[1]
        elif p[0] == "size" and sect:
            out.append((seg + "," + sect, int(p[1], 16))); sect = None
    return out

def loadcmds(b):
    return collections.Counter(l.split()[1] for l in run("otool", "-l", b).splitlines() if l.strip().startswith("cmd "))

def syms(b):
    rows = []
    for l in run("nm", b).splitlines():
        p = l.split()
        if len(p) == 3: rows.append((int(p[0], 16), p[1], p[2]))
        elif len(p) == 2: rows.append((None, p[0], p[1]))
    return rows

def fnsizes(rows, textend):
    t = sorted((a, n) for a, k, n in rows if k in "tT")
    cnt = collections.Counter(n for a, n in t)
    out = {}
    for i, (a, n) in enumerate(t):
        e = t[i + 1][0] if i + 1 < len(t) else textend
        if cnt[n] == 1: out[n] = e - a
    return out

def textend(b):
    for l in run("otool", "-l", b).split("Section")[1:]:
        if "sectname __text" in l:
            f = dict(x.split(None, 1) for x in l.strip().splitlines()[1:] if len(x.split(None, 1)) == 2)
            return int(f["addr"], 16) + int(f["size"], 16)

ship, ours = sys.argv[1], sys.argv[2]
rep = []
w = rep.append
import os
w(f"file size: shipped {os.path.getsize(ship)}  ours {os.path.getsize(ours)}  delta {os.path.getsize(ours) - os.path.getsize(ship)}")
lc_s, lc_o = loadcmds(ship), loadcmds(ours)
w("load commands: " + ("identical" if lc_s == lc_o else f"shipped {dict(lc_s)} ours {dict(lc_o)}"))
ss, so = dict(sections(ship)), dict(sections(ours))
w("sections (shipped / ours / delta):")
for k in sorted(set(ss) | set(so)):
    w(f"  {k:24s} {ss.get(k, 0):#10x} {so.get(k, 0):#10x} {so.get(k, 0) - ss.get(k, 0):+d}")
rs, ro = syms(ship), syms(ours)
us, uo = {n for a, k, n in rs if k == "U"}, {n for a, k, n in ro if k == "U"}
w(f"undefined: shipped {len(us)} ours {len(uo)}  only-shipped {sorted(us - uo)}  only-ours {sorted(uo - us)}")
es, eo = {n for a, k, n in rs if k in "TDSB"}, {n for a, k, n in ro if k in "TDSB"}
w(f"exported (global defined): shipped {len(es)} ours {len(eo)}  only-shipped {len(es - eo)}  only-ours {len(eo - es)}")
for n in sorted(es - eo)[:40]: w(f"  only-shipped export {n}")
for n in sorted(eo - es)[:40]: w(f"  only-ours export {n}")
ns, no = collections.Counter(n for a, k, n in rs if k != "U"), collections.Counter(n for a, k, n in ro if k != "U")
d_s, d_o = ns - no, no - ns
w(f"all defined symbols (with multiplicity): shipped {sum(ns.values())} ours {sum(no.values())}  only-shipped {sum(d_s.values())}  only-ours {sum(d_o.values())}")
for n, c in d_s.most_common(25): w(f"  only-shipped x{c} {n}")
for n, c in d_o.most_common(25): w(f"  only-ours x{c} {n}")
fs, fo = fnsizes(rs, textend(ship)), fnsizes(ro, textend(ours))
common = set(fs) & set(fo)
diff = sorted((fo[n] - fs[n], n) for n in common if fs[n] != fo[n])
w(f"functions with a unique name in both: {len(common)}  same size {len(common) - len(diff)}  different size {len(diff)}  total delta {sum(d for d, n in diff):+d} B")
for d, n in (diff if len(diff) <= 30 else diff[:15] + diff[-15:]): w(f"  {d:+6d} shipped {fs[n]:6d} {n}")
txt = "\n".join(rep) + "\n"
sys.stdout.write(txt)
if len(sys.argv) > 3: open(sys.argv[3], "w").write(txt)
