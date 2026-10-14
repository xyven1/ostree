#!/bin/bash
# One cold pull, timed. Read-only against the remote.
#   bench.sh LABEL URL REF [extra ostree pull args...]
# Env: OSTREE (default ./ostree), BENCH_ROOT (default /var/tmp/s3-bench),
#      BENCH_DEBUG=1 to capture curl verbose output and count connections,
#      BENCH_KEEP=1 to keep the repo.
set -uo pipefail
label=$1 url=$2 ref=$3
shift 3
ostree=${OSTREE:-./ostree}
root=${BENCH_ROOT:-/var/tmp/s3-bench}
export AWS_PROFILE=${AWS_PROFILE:?set AWS_PROFILE}
export AWS_DEFAULT_REGION=${AWS_DEFAULT_REGION:?set AWS_DEFAULT_REGION}
# aws-c-auth cannot refresh an SSO token; take role credentials from the CLI
# (which can) and pass them through the environment. Never printed.
eval "$(aws configure export-credentials --profile "${AWS_PROFILE}" --format env)"
unset AWS_PROFILE
export AWS_EC2_METADATA_DISABLED=true
mkdir -p "${root}"
repo="${root}/repo-${label}"
log="${root}/log-${label}.txt"
tim="${root}/time-${label}.txt"
rm -rf "${repo}"
"${ostree}" --repo="${repo}" init --mode=bare-user >/dev/null
"${ostree}" --repo="${repo}" remote add --set=gpg-verify=false origin "${url}" >/dev/null

dbg=()
if test -n "${BENCH_DEBUG:-}"; then export OSTREE_DEBUG_HTTP=1; fi
TIMEFORMAT='%R %U %S'
{ time "${ostree}" --repo="${repo}" pull --disable-verify-bindings "$@" origin "${ref}" >"${log}" 2>&1; } 2>"${tim}"
rc=$?
read -r wall user sys < "${tim}"
objects=$(find "${repo}/objects" -type f | wc -l)
bytes=$(du -sb "${repo}/objects" | cut -f1)
conns="-"
if test -n "${BENCH_DEBUG:-}"; then
    conns=$(grep -c -E '^\* (Connected to|Established connection to) ' "${log}" || true)
fi
awk -v l="${label}" -v w="${wall}" -v u="${user}" -v s="${sys}" -v o="${objects}" -v b="${bytes}" -v c="${conns}" -v rc="${rc}" \
    'BEGIN { printf "%-22s rc=%s wall=%7.1fs cpu=%6.1fs(u%.0f+s%.0f) objs=%6d %7.1f obj/s %6.1f MiB/s conns=%s\n", l, rc, w, u+s, u, s, o, o/w, b/1048576/w, c }'
if test "${rc}" -ne 0; then tail -3 "${log}"; fi
if test -z "${BENCH_KEEP:-}"; then rm -rf "${repo}"; fi
