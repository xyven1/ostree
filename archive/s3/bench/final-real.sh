#!/bin/bash
# Real-S3 A/B: pre-optimization build (99dd39d) vs current build, default settings,
# interleaved, with link samples. Read-only on the bucket.
B=$(dirname "$(realpath "$0")")
S=${S3_BENCH_URL:?s3:// URL of the benchmark repo}
OLD=/var/tmp/s3-bench/old-src/ostree
NEW=$PWD/ostree
run() { OSTREE="$1" bash "$B/bench.sh" "$2" "$S" "${@:3}"; }
case "$1" in
    a)
        for round in 1 2; do
            bash "$B/linknow.sh"
            run "$OLD" "sw-old-r${round}" ${S3_BENCH_REF_SW:?ref}
            run "$NEW" "sw-new-r${round}" ${S3_BENCH_REF_SW:?ref}
            run "$OLD" "delta-old-r${round}" ${S3_BENCH_REF_DELTA:?ref with a static delta} --require-static-deltas
            run "$NEW" "delta-new-r${round}" ${S3_BENCH_REF_DELTA:?ref with a static delta} --require-static-deltas
        done
        bash "$B/linknow.sh"
        ;;
    b)
        bash "$B/linknow.sh"
        run "$NEW" sys-new-r1 ${S3_BENCH_REF_SYSROOT:?ref}
        run "$OLD" sys-old-r1 ${S3_BENCH_REF_SYSROOT:?ref}
        bash "$B/linknow.sh"
        run "$NEW" sys-new-r2 ${S3_BENCH_REF_SYSROOT:?ref}
        bash "$B/linknow.sh"
        ;;
esac
