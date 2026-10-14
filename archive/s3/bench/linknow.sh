#!/bin/bash
# One quick sample of raw link capacity: 16 parallel streams of a 9.8 MiB object.
set -uo pipefail
export AWS_PROFILE=${AWS_PROFILE:?set AWS_PROFILE}
eval "$(aws configure export-credentials --format env)"
region=${AWS_DEFAULT_REGION:?set AWS_DEFAULT_REGION}
url="${S3_BENCH_HTTPS_OBJECT:?https URL of a ~10 MiB object}"
args=()
for i in $(seq 1 16); do args+=(-o /dev/null "${url}"); done
start=$(date +%s.%N)
curl -s --parallel --parallel-max 16 --parallel-immediate --http1.1 \
    --aws-sigv4 "aws:amz:${region}:s3" --user "${AWS_ACCESS_KEY_ID}:${AWS_SECRET_ACCESS_KEY}" \
    -H "x-amz-security-token: ${AWS_SESSION_TOKEN}" "${args[@]}"
end=$(date +%s.%N)
awk -v a="${start}" -v e="${end}" 'BEGIN { printf "link sample: 16 streams %.1f MiB/s\n", 16*10244741/1048576/(e-a) }'
