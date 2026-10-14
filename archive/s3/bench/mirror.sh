#!/bin/bash
# Build a local archive-mode mirror of the benchmark refs (read-only on S3).
set -uo pipefail
export AWS_PROFILE=${AWS_PROFILE:?set AWS_PROFILE}
export AWS_DEFAULT_REGION=${AWS_DEFAULT_REGION:?set AWS_DEFAULT_REGION}
m=/var/tmp/s3-bench/mirror/bkt/repo
S=${S3_BENCH_URL:?s3:// URL of the benchmark repo}
mkdir -p "$(dirname "${m}")"
if ! test -d "${m}/objects"; then
    ./ostree --repo="${m}" init --mode=archive >/dev/null
    ./ostree --repo="${m}" remote add --set=gpg-verify=false origin "${S}" >/dev/null
fi
for ref in ${S3_BENCH_REF_SW:?ref} ${S3_BENCH_REF_DELTA:?ref with a static delta} ${S3_BENCH_REF_SYSROOT:?ref}; do
    start=$(date +%s)
    ./ostree --repo="${m}" pull --mirror --disable-verify-bindings --disable-static-deltas \
        --max-outstanding-fetcher-requests=128 origin "${ref}" 2>&1 | tail -1
    echo "mirrored ${ref} in $(( $(date +%s) - start ))s"
done
aws s3 sync --only-show-errors "${S}/deltas/${S3_BENCH_DELTA:?delta dir}/" \
    "${m}/deltas/${S3_BENCH_DELTA:?delta dir}/"
./ostree --repo="${m}" refs
du -sh "${m}"; find "${m}/objects" -type f | wc -l; ls -la "${m}/deltas/${S3_BENCH_DELTA}" | head -5
