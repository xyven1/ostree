#!/bin/bash
# One simulator run with explicit knobs.
#   one.sh PROFILE REFKEY CONC WRITES DELTAPARTS [LABEL_SUFFIX]
# REFKEY: sw | sys | delta.  WRITES / DELTAPARTS: number, or "-" for the build's default.
B=$(dirname "$(realpath "$0")")
profile=$1 key=$2 conc=$3 writes=$4 parts=$5 suffix=${6:-}
extra=()
case "${key}" in
    sw) ref=${S3_BENCH_REF_SW:?ref} ;;
    sys) ref=${S3_BENCH_REF_SYSROOT:?ref} ;;
    delta) ref=${S3_BENCH_REF_DELTA:?ref with a static delta}; extra+=(--require-static-deltas) ;;
esac
if test "${conc}" != "-"; then extra+=(--max-outstanding-fetcher-requests=${conc}); fi
if test "${writes}" != "-"; then export OSTREE_EXP_MAX_WRITES=${writes}; fi
if test "${parts}" != "-"; then export OSTREE_EXP_MAX_DELTAPARTS=${parts}; fi
bash "$B/sim.sh" "${profile}" "${key}-c${conc}-w${writes}-p${parts}${suffix}" "${ref}" "${extra[@]}"
