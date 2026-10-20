#!/usr/bin/env python3
# codediff.py <shipped> <ours> [N]: compare every function's code in two NVRM binaries with llvm-objdump, addresses
# normalised: branch/call targets become <symbol+offset>, rip-relative operands become X(%rip) and their target is
# kept only when it is a named symbol (string/data layout may shift). Prints how many functions are identical.
# <wtl> fork: nvgfx | author: watchthelight </wtl>
import re, subprocess, sys, collections, os
OBJDUMP = os.environ.get("LLVM_OBJDUMP", "/opt/local/libexec/llvm-20/bin/llvm-objdump")
hdr = re.compile(r"^[0-9a-f]+ <(.+)>:$")
def funcs(b):
    p = subprocess.Popen([OBJDUMP, "-d", "--no-show-raw-insn", "--section=__text", b], stdout=subprocess.PIPE, text=True)
    res, seen, fn, cur = {}, collections.Counter(), None, None
    for l in p.stdout:
        l = l.rstrip("\n")
        m = hdr.match(l)
        if m:
            if fn: res.setdefault(fn, cur)
            fn = m.group(1); seen[fn] += 1; cur = []; continue
        if cur is None or ":" not in l: continue
        s = l.split(":", 1)[1].strip()
        if not s: continue
        s = re.sub(r"\s*## imm = .*", "", s)
        s = re.sub(r"0x[0-9a-f]+ <([^>]+)>", r"<\1>", s)
        s = re.sub(r"-?0x[0-9a-f]+\(%rip\)", "X(%rip)", s)
        s = re.sub(r"\s*#+\s*<[^>]+\+0x[0-9a-f]+>", "", s)   # rip target inside unnamed data (strings): layout-dependent
        s = re.sub(r"#+\s*<([^>]+)>", r"# <\1>", s)
        cur.append(re.sub(r"\s+", " ", s))
    if fn: res.setdefault(fn, cur)
    return {n: trim(v) for n, v in res.items() if seen[n] == 1}

def trim(v):
    # drop inter-function padding (nops from the assembler, zero fill from the linker, which decodes as add*, partly
    # running into the next function's first bytes) after the last terminator
    for i in range(len(v) - 1, -1, -1):
        if re.match(r"(retq|jmpq?|ud2)\b", v[i]):
            if all(re.match(r"(nop|add|int3)", x) for x in v[i + 1:]): return v[:i + 1]
            break
    while v and re.match(r"(nop|int3)", v[-1]): v = v[:-1]
    return v

a, b = funcs(sys.argv[1]), funcs(sys.argv[2])
common = sorted(set(a) & set(b))
diff = [n for n in common if a[n] != b[n]]
print(f"functions compared {len(common)}  identical (normalised) {len(common) - len(diff)}  different {len(diff)}")
for n in diff[:int(sys.argv[3]) if len(sys.argv) > 3 else 60]:
    print(f"  {n}  shipped {len(a[n])} insns, ours {len(b[n])}")
