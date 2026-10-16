#!/bin/bash
# seccmp.sh: REF=<shipped> NEW=<built> compare section sizes and contents (address column stripped)
for sec in "__TEXT __text" "__TEXT __cstring" "__TEXT __const" "__DATA __const" "__DATA __data" "__DATA __got"; do
  set -- $sec
  ra=$(otool -l "$REF" | grep -A4 "sectname $2" | grep -A3 "segname $1" | awk '/size/{print $2}' | head -1)
  na=$(otool -l "$NEW" | grep -A4 "sectname $2" | grep -A3 "segname $1" | awk '/size/{print $2}' | head -1)
  if [ "$2" = __cstring ]; then a=$(otool -v -s $1 $2 "$REF" | tail -n +3 | sed "s/^[0-9a-f]* *//" | md5); b=$(otool -v -s $1 $2 "$NEW" | tail -n +3 | sed "s/^[0-9a-f]* *//" | md5)
  else a=$(otool -s $1 $2 "$REF" | tail -n +3 | cut -f2- | md5); b=$(otool -s $1 $2 "$NEW" | tail -n +3 | cut -f2- | md5); fi
  printf '%-16s %-10s ref %-8s new %-8s %s\n' "$1" "$2" "$((ra))" "$((na))" "$([ "$a" = "$b" ] && echo identical || echo differs)"
done
