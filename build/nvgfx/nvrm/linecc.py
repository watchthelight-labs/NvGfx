#!/usr/bin/env python3
# linecc.py <tree> <template.c> <src.c> <out.o>: compile a C file with the exact compile_cmds.sh line NVIDIA's Makefile
# used for <template.c> in <tree> (src/nvidia or src/nvidia-modeset), swapping only source, object and depfile.
# Generalises kexts/NVRM/rmcc.py (which is fixed to src/nvidia + osapi.c) so modeset-internal glue such as
# nvrm_kapi_fields.c sees NVKMS's structs exactly as libnvkms.a did.
# <wtl> fork: nvgfx | author: watchthelight </wtl>
import os, shlex, subprocess, sys
tree, tmpl, src, out = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3]), os.path.abspath(sys.argv[4])
lines = [l for l in open(os.path.join(tree, "_out/Darwin_x86_64/compile_cmds.sh")) if f" -c {tmpl} " in l]
if len(lines) != 1: sys.exit(f"STOP: {len(lines)} lines for {tmpl} in compile_cmds.sh")
toks = shlex.split(lines[0].split(" && ")[0])
toks[toks.index(tmpl)] = src
for i, t in enumerate(toks):
    if t == "-o": toks[i + 1] = out
    if t in ("-MF", "-MT"): toks[i + 1] = out + ".d"
r = subprocess.run(toks, cwd=tree, capture_output=True, text=True)
sys.stderr.write(r.stderr[-6000:])
sys.exit(r.returncode)
