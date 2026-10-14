#!/bin/bash
# One cold pull against the local throttling server.
#   sim.sh PROFILE LABEL REF [extra ostree pull args...]
# PROFILE: wan  = what was measured to GovCloud from this host
#                 (45 ms/request, 100 ms connect, 1.5 MiB/s per connection, 22 MiB/s total)
#          fast = in-region-like (2 ms/request, unthrottled)
# Env: OSTREE (default ./ostree); any OSTREE_EXP_* pass through.
set -uo pipefail
profile=$1 label=$2 ref=$3
shift 3
B=$(dirname "$(realpath "$0")")
ostree=${OSTREE:-./ostree}
root=/var/tmp/s3-bench
port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
case "${profile}" in
    wan)  sargs=(--latency 0.045 --connect 0.1 --conn-rate ${SIM_CONN_RATE:-1572864} --total-rate 23068672) ;;
    fast) sargs=(--latency 0.002 --connect 0.004) ;;
    *) echo "unknown profile ${profile}" >&2; exit 2 ;;
esac
python3 "${B}/slowserver.py" "${root}/mirror" --port "${port}" --trace "${root}/trace-${label}.txt" "${sargs[@]}" &
spid=$!
trap 'kill ${spid} 2>/dev/null' EXIT
for i in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:${port}/__stats" && break; sleep 0.1; done

unset AWS_PROFILE AWS_REGION AWS_DEFAULT_REGION AWS_SESSION_TOKEN
export AWS_ACCESS_KEY_ID=simkey AWS_SECRET_ACCESS_KEY=simsecret AWS_EC2_METADATA_DISABLED=true
export AWS_CONFIG_FILE=${root}/none AWS_SHARED_CREDENTIALS_FILE=${root}/none2
repo="${root}/repo-${label}"
log="${root}/log-${label}.txt"
rm -rf "${repo}"
"${ostree}" --repo="${repo}" init --mode=bare-user >/dev/null
if test -n "${SIM_HTTP:-}"; then "${ostree}" --repo="${repo}" remote add --set=gpg-verify=false --set=http2=false origin "http://127.0.0.1:${port}/bkt/repo" >/dev/null; else
"${ostree}" --repo="${repo}" remote add --set=gpg-verify=false origin \
    "s3://bkt/repo?endpoint=http://127.0.0.1:${port}&region=us-east-1" >/dev/null; fi
TIMEFORMAT='%R %U %S'
{ time "${ostree}" --repo="${repo}" pull --disable-verify-bindings "$@" origin "${ref}" >"${log}" 2>&1; } 2>"${root}/time-${label}.txt"
rc=$?
read -r wall user sys < "${root}/time-${label}.txt"
st=$(curl -s "http://127.0.0.1:${port}/__stats")
awk -v l="${profile}/${label}" -v w="${wall}" -v u="${user}" -v s="${sys}" -v rc="${rc}" -v st="${st}" \
    'BEGIN { n = split(st, kv, " "); for (i = 1; i <= n; i++) { split(kv[i], p, "="); v[p[1]] = p[2] }
       printf "%-26s rc=%s wall=%6.1fs cpu=%5.1fs reqs=%6d %6.0f req/s %5.1f MiB/s conns=%4d max_open=%4d ranged=%d\n",
              l, rc, w, u+s, v["requests"], v["requests"]/w, v["bytes"]/1048576/w, v["connections"], v["max_open"], v["ranged"] }'
if test "${rc}" -ne 0; then tail -3 "${log}"; fi
rm -rf "${repo}"
