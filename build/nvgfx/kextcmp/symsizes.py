# symsizes.py <binary>: print "section symbol size" for every defined symbol, size = distance to the next symbol
# <wtl> fork: nvgfx | author: watchthelight </wtl>
import subprocess, sys, re
out = subprocess.run(["nm", "-nm", sys.argv[1]], capture_output=True, text=True).stdout
rows = []
for l in out.splitlines():
    m = re.match(r"([0-9a-f]+) \((\w+),(\w+)\) .*? (\S+)$", l)
    if m: rows.append((int(m.group(1), 16), m.group(2) + "," + m.group(3), m.group(4)))
for i, (a, sec, name) in enumerate(rows):
    nxt = next((b for b, s, _ in rows[i + 1:] if s == sec and b > a), None)
    print(sec, name, (nxt - a) if nxt else "end")
