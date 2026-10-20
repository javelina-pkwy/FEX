#!/bin/bash
# Native x86 timings of the same divide benchmarks FEX runs. ns per divide; ludiv_bench modes subtract a reps=0 run.
set -u
cd "$(dirname "$0")"
for b in dumb_div ludiv_bench idiv_bench; do gcc -O2 -o $b $b.c; done
ns() { local s=$(date +%s%N); "$@" > /dev/null; echo $(( $(date +%s%N) - s )); }
for t in 1 2 3 4 5; do
  echo "trial $t: $(./dumb_div | grep -oE '[0-9.]+ ns/divide') dumb_div | $(./idiv_bench div | grep -oE '[0-9.]+ ns/divide') long div | $(./idiv_bench idiv | grep -oE '[0-9.]+ ns/divide') long idiv"
  for m in tput:2000:4096 lat:2000:4096 tput10:2000:4096 lat10:2000:4096 u128fmt:400:1024; do
    IFS=: read -r mode reps per <<< "$m"
    t0=$(ns ./ludiv_bench $mode 0); t1=$(ns ./ludiv_bench $mode $reps)
    echo "  $mode $(echo "scale=2; ($t1 - $t0) / ($reps * $per)" | bc) ns per op"
  done
done
