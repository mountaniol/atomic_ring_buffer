#!/bin/bash
# A/B sweep: real-library single (v2) vs burst policies (v3), both builds.
# usage: v3_ab_sweep.sh <out.csv> [cpu_p cpu_c]
OUT=${1:?out.csv}
CP=${2:-2}; CC=${3:-4}
MS=300; WARM=150

echo "build,policy,rate_M,thr_M,drop_pct,chunk,p50,p90,p99,p999,max,mean,samples" > "$OUT"
for r in 1 2 3; do
    for build in line idx; do
        for lam in 5 10 20 50 100 150 200 250 300 400; do
            for pol in single adapt b8 b32 b128; do
                ./v3_ab_${build}.out $CP $CC $pol $lam $MS $WARM \
                    | sed "s/^/${build},/" >> "$OUT"
                sleep 0.3
            done
        done
    done
done
