#!/bin/bash
#
# SPDX-License-Identifier: LGPL-2.0+
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

# Repo safety under failure for s3:// pulls, in particular for large objects
# fetched as several ranged requests. Whatever the server does, a failed pull
# must leave no partial or mixed object in the repo, must leave the commit
# marked partial, and must be recoverable by simply pulling again.

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_ostree_feature aws
if ! command -v python3 >/dev/null; then
    skip "python3 is required"
fi

echo '1..10'

cd ${test_tmpdir}

unset AWS_PROFILE AWS_REGION AWS_DEFAULT_REGION AWS_SESSION_TOKEN AWS_ROLE_ARN \
      AWS_WEB_IDENTITY_TOKEN_FILE AWS_CONTAINER_CREDENTIALS_FULL_URI \
      AWS_CONTAINER_CREDENTIALS_RELATIVE_URI
export AWS_EC2_METADATA_DISABLED=true
export AWS_CONFIG_FILE=${test_tmpdir}/aws-config-absent
export AWS_SHARED_CREDENTIALS_FILE=${test_tmpdir}/aws-credentials-absent
export AWS_ACCESS_KEY_ID=faultkey AWS_SECRET_ACCESS_KEY=faultsecret

# --- Server-side repo: a few small files and one incompressible 12 MiB file,
# --- which is fetched as three 4 MiB ranges.
mkdir -p files/sub
for i in $(seq 1 20); do
    echo "content ${i}" > files/sub/file-${i}
done
head -c 12582912 /dev/urandom > files/big.bin
mkdir -p root/bkt
ostree_repo_init root/bkt/repo --mode=archive
${CMD_PREFIX} ostree --repo=root/bkt/repo commit -b main -s c --tree=dir=files >/dev/null
${CMD_PREFIX} ostree --repo=root/bkt/repo summary -u
rev=$(${CMD_PREFIX} ostree --repo=root/bkt/repo rev-parse main)

port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
echo ok > mode
: > requests.log
python3 $(dirname $0)/s3-fault-server.py ${test_tmpdir}/root ${test_tmpdir}/mode \
        ${test_tmpdir}/requests.log ${port} > server.log 2>&1 &
server_pid=$!
kill_server () {
    kill ${server_pid} 2>/dev/null || true
}
libtest_exit_cmds+=(kill_server)
for i in $(seq 1 100); do
    if curl -sf -o /dev/null http://127.0.0.1:${port}/bkt/repo/config; then
        break
    fi
    sleep 0.1
done
curl -sf -o /dev/null http://127.0.0.1:${port}/bkt/repo/config || fatal "fault server did not start"

s3url="s3://bkt/repo?endpoint=http://127.0.0.1:${port}&region=us-east-1"

set_mode () {
    echo $1 > mode
    : > requests.log
}

new_client_repo () {
    rm -rf $1
    ostree_repo_init $1 "${@:2}"
    ${CMD_PREFIX} ostree --repo=$1 remote add --set=gpg-verify=false origin "${s3url}"
}

# Number of objects in a repo that are large enough to be (part of) big.bin.
count_big_objects () {
    find $1/objects -type f -size +2M | wc -l
}

# Whether the follow-up ranges of the big object were requested.
ranges_requested () {
    grep -q '^4194304-8388607 ' requests.log && grep -q '^8388608-12' requests.log
}

assert_pull_ok () {
    ${CMD_PREFIX} ostree --repo=$1 pull --disable-static-deltas "${@:2}" origin main
    assert_streq "$(${CMD_PREFIX} ostree --repo=$1 rev-parse main)" "${rev}"
    assert_not_has_file $1/state/${rev}.commitpartial
    ${CMD_PREFIX} ostree --repo=$1 fsck >/dev/null
    test "$(count_big_objects $1)" -eq 1 || fatal "expected the big object to be present in $1"
}

# A pull under a persistent fault must fail, leave no trace of the big object,
# leave the commit marked partial, and leave a repo that fsck accepts.
assert_pull_fails_cleanly () {
    if ${CMD_PREFIX} ostree --repo=$1 pull --disable-static-deltas --network-retries=0 \
                     "${@:2}" origin main 2>err.txt; then
        fatal "pull succeeded in mode $(cat mode)"
    fi
    test "$(count_big_objects $1)" -eq 0 \
        || fatal "a partial or corrupt big object was left in $1 (mode $(cat mode))"
    assert_has_file $1/state/${rev}.commitpartial
    ${CMD_PREFIX} ostree --repo=$1 fsck >/dev/null
}

# 1: no faults; the big object arrives in ranges and is intact
new_client_repo repo1
set_mode ok
assert_pull_ok repo1
ranges_requested || fatal "big object was not fetched in ranges"
${CMD_PREFIX} ostree --repo=repo1 checkout main checkout1
cmp files/big.bin checkout1/big.bin
echo "ok ranged pull"

# 2: a follow-up range fails outright; then the server recovers
new_client_repo repo2
set_mode child500
assert_pull_fails_cleanly repo2
set_mode ok
assert_pull_ok repo2
echo "ok failed range, then recovery"

# 3: a follow-up range is cut off halfway
new_client_repo repo3
set_mode childshort
assert_pull_fails_cleanly repo3
set_mode ok
assert_pull_ok repo3
echo "ok truncated range, then recovery"

# 4: the server ignores Range on follow-ups and sends the whole object
new_client_repo repo4
set_mode child200
assert_pull_fails_cleanly repo4
echo "ok range ignored by server"

# 5: a follow-up range carries the wrong bytes; ostree's checksum catches it
new_client_repo repo5
set_mode childgarbage
assert_pull_fails_cleanly repo5
echo "ok corrupt range rejected by checksum"

# 6: the object changes between the first part and the follow-ups
new_client_repo repo6
set_mode etagchange
assert_pull_fails_cleanly repo6
# every follow-up range must have been conditional on the first part's ETag
if grep '^4194304-' requests.log | grep -q -v ' "v1-'; then
    cat requests.log
    fatal "a follow-up range was sent without If-Match"
fi
echo "ok changed object not stitched together"

# 7: without an ETag there is nothing to tie the parts together, so the
#    object must be fetched in one piece
new_client_repo repo7
set_mode noetag
assert_pull_ok repo7
if grep -q '^4194304-' requests.log; then
    cat requests.log
    fatal "follow-up ranges were sent although the server gave no ETag"
fi
echo "ok no ETag, no ranges"

# 8: a weak ETag cannot be used with If-Match either
new_client_repo repo8
set_mode weaketag
assert_pull_ok repo8
if grep -q '^4194304-' requests.log; then
    cat requests.log
    fatal "follow-up ranges were sent although the server gave only a weak ETag"
fi
echo "ok weak ETag, no ranges"

# 9: --http-trusted mirroring links the fetched file into the repo without
#    checksumming it, so the assembled file itself has to be right; fsck
#    then verifies it independently
new_client_repo mirror9 --mode=archive
set_mode ok
${CMD_PREFIX} ostree --repo=mirror9 pull --mirror --http-trusted --disable-static-deltas origin main
ranges_requested || fatal "big object was not fetched in ranges"
${CMD_PREFIX} ostree --repo=mirror9 fsck >/dev/null
test "$(count_big_objects mirror9)" -eq 1 || fatal "big object missing from mirror"
echo "ok trusted mirror with ranges"

# 10: the same unverified path under a fault must not link a partial file
new_client_repo mirror10 --mode=archive
set_mode childshort
if ${CMD_PREFIX} ostree --repo=mirror10 pull --mirror --http-trusted --disable-static-deltas \
                 --network-retries=0 origin main 2>err.txt; then
    fatal "trusted mirror pull succeeded with a truncated range"
fi
test "$(count_big_objects mirror10)" -eq 0 || fatal "a partial object was linked into the mirror"
set_mode ok
${CMD_PREFIX} ostree --repo=mirror10 pull --mirror --http-trusted --disable-static-deltas origin main
${CMD_PREFIX} ostree --repo=mirror10 fsck >/dev/null
test "$(count_big_objects mirror10)" -eq 1 || fatal "big object missing from mirror after recovery"
echo "ok trusted mirror never links a partial file"
