#!/bin/bash
# compare.sh: SHIPPED=<2.1.0 pkgroot> [OUTROOT=<build out>] compare.sh. Compares every built kext with the shipped 2.1.0 one (symbols, sections, data symbol sizes, vtables, plist)
# <wtl> fork: nvgfx | author: watchthelight </wtl>
K=${OUTROOT:-$HOME/src/NvGfx-build/kexts}; T=$(cd "$(dirname "$0")" && pwd); P=${SHIPPED:?set SHIPPED to the 2.1.0 pkgroot}
cmp1() {  # name builtdir shippedkext
  local n=$1 d=$2 rk=$3; local N=$d/$n.kext/Contents/MacOS/$n R=$rk/Contents/MacOS/$n
  echo "===== $n ($d)  ref $(stat -f %z $R) B  new $(stat -f %z $N) B"
  nm -gU $R | awk '{print $3}' | sort > $d/ref.exp; nm -gU $N | awk '{print $3}' | sort > $d/new.exp
  nm -u $R | sort > $d/ref.und; nm -u $N | sort > $d/new.und
  echo "exports: ref $(wc -l <$d/ref.exp) new $(wc -l <$d/new.exp), diff lines $(diff $d/ref.exp $d/new.exp | grep -c '^[<>]')"
  diff $d/ref.exp $d/new.exp | grep '^[<>]' | head -10
  echo "undefined: ref $(wc -l <$d/ref.und) new $(wc -l <$d/new.und), diff lines $(diff $d/ref.und $d/new.und | grep -c '^[<>]')"
  diff $d/ref.und $d/new.und | grep '^[<>]' | head -20
  REF=$R NEW=$N $T/seccmp.sh
  python3 -I $T/symsizes.py $R | grep -v __text | sort > $d/ref.sizes; python3 -I $T/symsizes.py $N | grep -v __text | sort > $d/new.sizes
  echo "data symbols with different size: $(diff $d/ref.sizes $d/new.sizes | grep -c '^<')"; diff $d/ref.sizes $d/new.sizes | head -10
  python3 -I $T/vtables.py $R > $d/ref.vt; python3 -I $T/vtables.py $N > $d/new.vt
  echo "vtable slots: ref $(wc -l <$d/ref.vt) new $(wc -l <$d/new.vt), diff lines $(diff $d/ref.vt $d/new.vt | grep -c '^[<>]')"; diff $d/ref.vt $d/new.vt | head -10
  cmp -s $rk/Contents/Info.plist $d/$n.kext/Contents/Info.plist && echo "Info.plist identical" || echo "Info.plist DIFFERS"
}
cmp1 NVRMFB $K/NVRMFB $P/Library/Extensions/NVRMFB.kext
for m in 15 26; do cmp1 NVAccel $K/NVAccel/$m $P/Library/NullMoth/kexts/$m/NVAccel.kext; done
for m in 15 26; do cmp1 NMXAccel $K/NMXAccel/$m $P/Library/NullMoth/kexts/$m/NMXAccel.kext; done
