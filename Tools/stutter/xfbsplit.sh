#!/bin/sh
# A picture copied to the XFB in two halves is shown whole: xfbsplit.c
# writes a GameCube program whose top half is red and bottom half blue,
# copied to the XFB separately as a game larger than the EFB copies its
# picture, with XFB copies kept only in VRAM (the default). The core runs
# it and the last picture's colour is read in the middle of each half.
#
#    Tools/stutter/xfbsplit.sh <core> [frames]
#
# Exits 0 and prints PASS when the top is red and the bottom blue, 1 and
# FAIL otherwise (a black picture is the XFB taken for not there).
# BACKENDS (vulkan) lists the renderers to check, vulkan and/or software.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CORE=$1
FRAMES=${2:-120}
OUT=${OUT:-$PWD/stutter-out}
BACKENDS=${BACKENDS:-vulkan}
CC=${CC:-cc}

if [ -z "$CORE" ]; then
   echo "usage: $0 <core> [frames]" >&2
   exit 2
fi
case "$CORE" in /*) ;; *) CORE=$PWD/$CORE ;; esac

mkdir -p "$OUT"
case "$(uname -s)" in
   MINGW*|MSYS*) EXE=.exe; LIBS= ;;
   *) EXE=; LIBS="-ldl -lpthread" ;;
esac
INC="-isystem $ROOT/Externals/libretro-common/include -isystem $ROOT/Externals/Vulkan-Headers/include"
$CC -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 $INC -o "$OUT/stutter$EXE" \
   "$HERE/stutter.c" "$HERE/allocs.c" $LIBS
$CC -std=c89 -pedantic -Wall -Wextra -O2 -o "$OUT/xfbsplit$EXE" "$HERE/xfbsplit.c"
"$OUT/xfbsplit$EXE" "$OUT/xfbsplit.dol" > /dev/null

# a colour is red (or blue) when that channel is over 0xa0 and the other
# two under 0x60
is() {
   r=$(printf %d 0x$(echo "$2" | cut -c1-2))
   g=$(printf %d 0x$(echo "$2" | cut -c3-4))
   b=$(printf %d 0x$(echo "$2" | cut -c5-6))
   case $1 in
      red) [ $r -gt 160 ] && [ $g -lt 96 ] && [ $b -lt 96 ] ;;
      blue) [ $b -gt 160 ] && [ $r -lt 96 ] && [ $g -lt 96 ] ;;
   esac
}

status=0
for backend in $BACKENDS; do
   dir="$OUT/xfbsplit-$backend"
   rm -rf "$dir"
   mkdir -p "$dir/system/dolphin-emu" "$dir/save"
   cp -r "$ROOT/Data/Sys" "$dir/system/dolphin-emu/Sys"
   "$OUT/stutter$EXE" "$CORE" "$OUT/xfbsplit.dol" --backend "$backend" --frames "$FRAMES" \
      --check --probe --system "$dir/system" --save "$dir/save" \
      --opt dolphin_main_cpu_thread=disabled > "$dir/run.txt" 2> "$dir/log.txt" || true
   last=$(grep '^probe ' "$dir/run.txt" | tail -n 1)
   top=$(echo "$last" | sed -n 's/.*top=\([0-9a-f]*\).*/\1/p')
   bottom=$(echo "$last" | sed -n 's/.*bottom=\([0-9a-f]*\).*/\1/p')
   if [ -n "$top" ] && is red "$top" && is blue "$bottom"; then
      echo "xfbsplit $backend: PASS (top $top, bottom $bottom)"
   else
      echo "xfbsplit $backend: FAIL (top ${top:-none}, bottom ${bottom:-none}; see $dir)"
      status=1
   fi
done
exit $status
