#!/bin/sh
# run.sh: the stutter gate (AGENTS.md, "Shader compilation and stutter").
#
#    Tools/stutter/run.sh <candidate core> [parent core]
#
# Both cores are stats builds (make STATS=1). Builds the harness and the
# corpus, then, for each backend:
#   - runs the corpus in four cache states, each in its own directories:
#     cold (no cache, the default), recipes (uid caches only), warm (every
#     cache), both with the shader cache on, and state (a savestate from
#     mid-corpus loaded into a fresh process);
#   - with a parent core, times the candidate and the parent ABBA, and
#     compares their pictures (separate --check runs, in each synchronous
#     compilation mode);
#   - checks the hard gates against baseline.txt and the parent: draw-path
#     compiles, waits and disk writes, and shaders and pipelines built,
#     each over the whole run (worker compiles are reported, not gated:
#     how many a worker finishes before the end depends on timing).
# Exits nonzero when a gate fails.
#
# Environment: BACKENDS (vulkan; software too, slow), FRAMES (runs at most), NEW (two passes of the
# corpus), OUT (./stutter-out), CC (cc), BASELINE (Tools/stutter/baseline.txt),
# UPDATE_BASELINE=1 writes the candidate's numbers where they are lower,
# CREATE_SPREAD (2) is the run-to-run spread allowed in shaders and
# pipelines built.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CAND=$1
PARENT=$2
OUT=${OUT:-$PWD/stutter-out}
BACKENDS=${BACKENDS:-vulkan}
CC=${CC:-cc}
BASELINE=${BASELINE:-$HERE/baseline.txt}
CREATE_SPREAD=${CREATE_SPREAD:-2}

if [ -z "$CAND" ]; then
   echo "usage: $0 <candidate stats core> [parent stats core]" >&2
   exit 2
fi
case "$CAND" in /*) ;; *) CAND=$PWD/$CAND ;; esac
if [ -n "$PARENT" ]; then
   case "$PARENT" in /*) ;; *) PARENT=$PWD/$PARENT ;; esac
fi

mkdir -p "$OUT"
rm -f "$OUT"/abba-*
case "$(uname -s)" in
   MINGW*|MSYS*) EXE=.exe; LIBS= ;;
   *) EXE=; LIBS="-ldl -lpthread" ;;
esac
INC="-isystem $ROOT/Externals/libretro-common/include -isystem $ROOT/Externals/Vulkan-Headers/include"
$CC -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 -o "$OUT/gxgen$EXE" "$HERE/gxgen.c"
$CC -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 $INC -o "$OUT/stutter$EXE" \
   "$HERE/stutter.c" $LIBS
"$OUT/gxgen$EXE" "$OUT/corpus.dff" 2 > "$OUT/corpus.txt"
COMBOS=$(sed -n 's/.*: \([0-9]*\) combinations.*/\1/p' "$OUT/corpus.txt")
# the corpus's pictures, two passes; runs, at most, to get there
NEW=${NEW:-$((COMBOS * 2))}
FRAMES=${FRAMES:-$((NEW * 8))}
# the state is saved where the second pass starts

# fresh frontend directories: the core's Sys data, nothing else
fresh() {
   rm -rf "$1"
   mkdir -p "$1/system/dolphin-emu" "$1/save"
   cp -r "$ROOT/Data/Sys" "$1/system/dolphin-emu/Sys"
}

# run <core> <backend> <dirs> <result> [harness options]
run() {
   core=$1; backend=$2; dirs=$3; result=$4
   shift 4
   "$OUT/stutter$EXE" "$core" "$OUT/corpus.dff" --backend "$backend" --frames "$FRAMES" \
      --new "$NEW" \
      --system "$dirs/system" --save "$dirs/save" "$@" > "$result" 2> "$result.log" || {
      echo "FAIL: the harness failed, $result.log" >&2
      cat "$result.log" >&2
      exit 1
   }
}

# the four cache states for one core and backend, results under <tag>
states() {
   core=$1; backend=$2; tag=$3
   d=$OUT/dirs-$tag-$backend
   fresh "$d"
   run "$core" "$backend" "$d" "$OUT/$tag-$backend-cold.txt" --boot 1
   # the shader cache, off by default, filled by a run with it on
   run "$core" "$backend" "$d" "$OUT/$tag-$backend-fill.txt" --boot 1 \
      --opt dolphin_shader_cache=enabled
   # recipes: the uid caches kept, the shader binaries and driver blobs gone
   rm -rf "$d/system/dolphin-emu/User/Cache/Shaders"
   find "$d" -name '*.cache' -exec rm -f {} + 2>/dev/null || true
   run "$core" "$backend" "$d" "$OUT/$tag-$backend-recipes.txt" --boot 1 \
      --opt dolphin_shader_cache=enabled
   run "$core" "$backend" "$d" "$OUT/$tag-$backend-warm.txt" --boot 1 \
      --opt dolphin_shader_cache=enabled \
      --state-new "$COMBOS" --state-out "$OUT/$tag-$backend.state"
   # into a fresh process, with fresh caches: what loading a state costs
   ds=$OUT/dirs-$tag-$backend-state
   fresh "$ds"
   run "$core" "$backend" "$ds" "$OUT/$tag-$backend-state.txt" --boot 1 \
      --state-in "$OUT/$tag-$backend.state"
}

# each corpus frame of a --check result, with the pictures it was, sorted
# and joined by commas: a frame may be presented again with what follows
# partly drawn. Not the last, presented as the player starts over.
picture_map() {
   sed -n 's/^picture .* corpus=\([0-9]*\) .* hash=\([0-9a-f]*\)/\1 \2/p' "$1" |
      awk -v last=$((NEW - 1)) '$1 != last' | sort -u |
      awk 'NR == 1 || $1 != k { if (NR > 1) print k, v; k = $1; v = $2; next } { v = v "," $2 }
           END { if (NR) print k, v }'
}

# the frames of a joined pair of picture maps that differ: whose pictures
# have none in common (a frame one run presents twice, another may once)
pictures_differ() {
   awk '{ n = split($2, a, ","); split($3, b, ","); same = 0
          for (i = 1; i <= n; i++) for (j in b) if (a[i] == b[j]) same = 1
          if (!same) print }' "$1"
}

# boot and play counts of a counter in a result
stat() {
   sed -n "s/^stat $2 boot=\([0-9]*\) boot_mcyc=[0-9.]* play=\([0-9]*\) .*/\1 \2/p" "$1"
}

FAILED=0
for backend in $BACKENDS; do
   echo "== $backend"
   states "$CAND" "$backend" cand
   if [ -n "$PARENT" ]; then
      states "$PARENT" "$backend" parent
      # ABBA timing of the warm and cold runs, cold in fresh directories
      for order in cand parent parent cand; do
         core=$CAND
         [ "$order" = parent ] && core=$PARENT
         # with the shader cache on, which the cold run fills for the warm
         # one: what it writes, it writes as the core closes
         fresh "$OUT/dirs-abba"
         run "$core" "$backend" "$OUT/dirs-abba" "$OUT/abba-$order-$backend-cold.txt.$$" \
            --opt dolphin_shader_cache=enabled
         cat "$OUT/abba-$order-$backend-cold.txt.$$" >> "$OUT/abba-$order-$backend-cold.txt"
         run "$core" "$backend" "$OUT/dirs-abba" "$OUT/abba-$order-$backend-warm.txt.$$" \
            --opt dolphin_shader_cache=enabled
         cat "$OUT/abba-$order-$backend-warm.txt.$$" >> "$OUT/abba-$order-$backend-warm.txt"
         rm -f "$OUT"/abba-*.$$
      done
      # pictures: the same from both cores, frame by frame. Each picture
      # carries its corpus frame's number (gxgen); a run presents most
      # frames, and the frames both runs present are compared. Each
      # synchronous mode draws a frame with the same pipeline every run,
      # ubershaders (1) and specialized (0); the asynchronous modes draw
      # with whichever is ready, which timing decides.
      for mode in 1 0; do
         for tag in cand parent; do
            core=$CAND
            [ "$tag" = parent ] && core=$PARENT
            fresh "$OUT/dirs-check"
            # with the copy filter on, as the corpus's copies set it, and
            # textures decoded on the CPU, the reference for those on the GPU
            run "$core" "$backend" "$OUT/dirs-check" "$OUT/check-$tag-$backend-$mode.txt" \
               --check --opt dolphin_shader_compilation_mode=$mode \
               --opt dolphin_disable_copy_filter=disabled \
               --opt dolphin_gpu_texture_decoding=disabled
            picture_map "$OUT/check-$tag-$backend-$mode.txt" > "$OUT/check-$tag-$backend-$mode.map"
         done
         join "$OUT/check-cand-$backend-$mode.map" "$OUT/check-parent-$backend-$mode.map" \
            > "$OUT/check-$backend-$mode.joined"
         both=$(wc -l < "$OUT/check-$backend-$mode.joined")
         differ=$(pictures_differ "$OUT/check-$backend-$mode.joined" | wc -l)
         echo "   pictures, compilation mode $mode: $both frames in both runs, $differ differ"
         if [ "$differ" != 0 ] || [ "$both" -lt $((NEW / 3)) ]; then
            echo "FAIL $backend: the pictures differ from the parent's (compilation mode $mode)"
            FAILED=1
         fi
      done
   fi

   # textures decoded on the GPU: the same pictures as the candidate's
   # decoded on the CPU, with the picture checks
   if [ -n "$PARENT" ]; then
      fresh "$OUT/dirs-check"
      run "$CAND" "$backend" "$OUT/dirs-check" "$OUT/check-cand-$backend-gpudecode.txt" \
         --check --opt dolphin_shader_compilation_mode=0 \
         --opt dolphin_disable_copy_filter=disabled --opt dolphin_gpu_texture_decoding=enabled
      picture_map "$OUT/check-cand-$backend-gpudecode.txt" > "$OUT/check-cand-$backend-gpudecode.map"
      join "$OUT/check-cand-$backend-gpudecode.map" "$OUT/check-cand-$backend-0.map" \
         > "$OUT/check-$backend-gpudecode.joined"
      both=$(wc -l < "$OUT/check-$backend-gpudecode.joined")
      differ=$(pictures_differ "$OUT/check-$backend-gpudecode.joined" | wc -l)
      echo "   pictures, GPU texture decoding: $both frames in both runs, $differ differ"
      if [ "$differ" != 0 ] || [ "$both" -lt $((NEW / 3)) ]; then
         echo "FAIL $backend: textures decoded on the GPU differ from those decoded on the CPU"
         FAILED=1
      fi
   fi

   for state in cold recipes warm state; do
      r=$OUT/cand-$backend-$state.txt
      grep -q '^stat ' "$r" || { echo "FAIL $backend: no counters, not a stats core"; exit 1; }
      sed -n 's/^\(timing\|frames\) //p' "$r" | sed "s/^/   $state: /"
      summary=""
      # hard gates: each counter against the baseline, and against the parent
      for kind in draw_gx_pipeline draw_uber_pipeline draw_efb_copy_to_vram \
                  draw_efb_copy_to_ram draw_reinterpret draw_decode draw_queued_compile \
                  draw_wait draw_disk_write draw_driver_pipeline shader_create pipeline_create \
                  driver_pipeline; do
         set -- $(stat "$r" "$kind")
         [ $# = 2 ] || { echo "FAIL $backend $state: no $kind counter"; FAILED=1; continue; }
         boot=$1; play=$2
         key="$backend $state $kind"
         base=$(sed -n "s/^$key \([0-9]*\)$/\1/p" "$BASELINE" 2>/dev/null || true)
         # the whole run, boot and play: where boot ends races the GPU thread
         value=$((boot + play))
         # a shader or pipeline the draw path builds while a worker builds the
         # same one is made twice; until the draw path builds none, that race
         # moves these by a few
         slack=0
         case "$kind" in shader_create|pipeline_create|driver_pipeline|draw_driver_pipeline) slack=$CREATE_SPREAD ;; esac
         if [ -n "$base" ] && [ "$value" -gt $((base + slack)) ]; then
            echo "FAIL $key: $value, baseline $base"
            FAILED=1
         elif [ -n "$base" ] && [ "$value" -lt "$base" ]; then
            echo "   better $key: $value, baseline $base (lower it)"
         fi
         # a parent from before a kind was counted has nothing to compare
         set -- $( [ -n "$PARENT" ] && stat "$OUT/parent-$backend-$state.txt" "$kind")
         if [ $# = 2 ]; then
            pvalue=$(($1 + $2))
            if [ "$value" -gt $((pvalue + slack)) ]; then
               echo "FAIL $key: $value, parent $pvalue"
               FAILED=1
            fi
         fi
         echo "$key $value" >> "$OUT/measured.txt.$$"
         [ "$value" != 0 ] && summary="$summary $kind=$value"
      done
      echo "   $state:$summary"
   done
   if [ -n "$PARENT" ]; then
      for state in cold warm; do
         for tag in parent cand; do
            awk -v t="$tag" -v s="$state" '/^timing /{
                  for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = v[kv[1]] " " kv[2] }
               } END {
                  printf "   %s %s: load_ms%s | boot_ms%s | p50%s | p99%s | max%s | over%s\n", s, t,
                     v["load_ms"], v["boot_ms"], v["run_p50_ms"], v["run_p99_ms"], v["run_max_ms"], v["over_budget"]
               }' "$OUT/abba-$tag-$backend-$state.txt"
         done
      done
   fi
done

if [ "$UPDATE_BASELINE" = 1 ]; then
   # only ever lower: a number above the baseline is a failure, not an update
   sort "$OUT/measured.txt.$$" | while read -r b s k v; do
      base=$(sed -n "s/^$b $s $k \([0-9]*\)$/\1/p" "$BASELINE" 2>/dev/null || true)
      # a few fewer shaders or pipelines built is the race, not a gain
      slack=0
      case "$k" in shader_create|pipeline_create|driver_pipeline|draw_driver_pipeline) slack=$CREATE_SPREAD ;; esac
      if [ -z "$base" ] || [ $((v + slack)) -lt "$base" ]; then
         echo "$b $s $k $v"
      else
         echo "$b $s $k $base"
      fi
   done > "$BASELINE.new"
   mv "$BASELINE.new" "$BASELINE"
   echo "baseline: $BASELINE"
fi
rm -f "$OUT/measured.txt.$$"

[ "$FAILED" = 0 ] && echo "PASS" || echo "FAIL"
exit $FAILED
