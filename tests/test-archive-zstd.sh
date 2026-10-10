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

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_ostree_feature zstd

echo '1..20'

setup_test_repository "archive-zstd"

# 11 tests shared with test-archivez.sh
. ${test_srcdir}/archive-test.sh

cd ${test_tmpdir}
assert_file_has_content repo/config '^mode=archive-zstd$'
find repo/objects -name '*.filez' > filez.txt
assert_not_file_has_content filez.txt .
find repo/objects -name '*.filezst' > filezst.txt
assert_file_has_content filezst.txt 'filezst$'
echo "ok objects are named .filezst"

# The payload after the header is one standard zstd frame
objpath=$(ostree_file_path_to_relative_object_path repo test2 /baz/cow)
assert_str_match "${objpath}" '\.filezst$'
python3 - repo/${objpath} <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read()
(hdrlen,) = struct.unpack('>I', data[:4])
payload = data[8 + hdrlen:]
assert payload[:4] == b'\x28\xb5\x2f\xfd', payload[:4]
open('cow.zst', 'wb').write(payload)
EOF
if command -v zstd >/dev/null; then
    zstd -q -d cow.zst -o cow.out
    assert_file_has_content cow.out '^moo$'
fi
echo "ok payload is a standard zstd frame"

$OSTREE fsck -q
echo "ok fsck"

# Empty files, symlinks, incompressible data and a multi-megabyte file
mkdir edge
: > edge/empty
ln -s empty edge/link
head -c 3000000 /dev/urandom > edge/random
python3 -c "import sys; sys.stdout.write('ostree zstd ' * 500000)" > edge/big
$OSTREE commit -b edge --tree=dir=edge >/dev/null
$OSTREE fsck -q
$OSTREE checkout -U edge edge-co
assert_has_file edge-co/empty
test ! -s edge-co/empty
assert_symlink_has_content edge-co/link '^empty$'
cmp edge/random edge-co/random
cmp edge/big edge-co/big
echo "ok empty file, symlink, random and large content round trip"

# zstd-level: out-of-range and junk values are clamped, not fatal
for level in 1 19 0 99 abc; do
    rm -rf repo-level co-level
    ostree_repo_init repo-level --mode=archive-zstd
    ${CMD_PREFIX} ostree --repo=repo-level config set archive.zstd-level ${level}
    ${CMD_PREFIX} ostree --repo=repo-level commit -b edge --tree=dir=edge >/dev/null
    ${CMD_PREFIX} ostree --repo=repo-level fsck -q
    ${CMD_PREFIX} ostree --repo=repo-level checkout -U edge co-level
    cmp edge/big co-level/big
    du -sb repo-level/objects | cut -f1 > size-${level}.txt
done
# Level 19 must compress the compressible file at least as well as level 1
test "$(cat size-19.txt)" -le "$(cat size-1.txt)"
echo "ok zstd-level is honoured and clamped"

# A truncated object must be an error, never a short file
rm -rf repo-trunc co-trunc
ostree_repo_init repo-trunc --mode=archive-zstd
${CMD_PREFIX} ostree --repo=repo-trunc commit -b edge --tree=dir=edge >/dev/null
bigobj=$(ostree_file_path_to_relative_object_path repo-trunc edge /random)
size=$(stat -c %s repo-trunc/${bigobj})
truncate -s $((size / 2)) repo-trunc/${bigobj}
if ${CMD_PREFIX} ostree --repo=repo-trunc fsck -q 2>err.txt; then
    assert_not_reached "fsck passed on a truncated .filezst"
fi
if ${CMD_PREFIX} ostree --repo=repo-trunc checkout -U edge co-trunc 2>err.txt; then
    assert_not_reached "checkout succeeded from a truncated .filezst"
fi
echo "ok truncated object is detected"

# A corrupted payload must be an error
rm -rf repo-corrupt
ostree_repo_init repo-corrupt --mode=archive-zstd
${CMD_PREFIX} ostree --repo=repo-corrupt commit -b edge --tree=dir=edge >/dev/null
bigobj=$(ostree_file_path_to_relative_object_path repo-corrupt edge /random)
size=$(stat -c %s repo-corrupt/${bigobj})
printf '\xff\xff\xff\xff\xff\xff\xff\xff' | dd of=repo-corrupt/${bigobj} bs=1 seek=$((size / 2)) conv=notrunc status=none
if ${CMD_PREFIX} ostree --repo=repo-corrupt fsck -q 2>err.txt; then
    assert_not_reached "fsck passed on a corrupted .filezst"
fi
echo "ok corrupted object is detected"

# A zlib payload under a .filezst name is rejected
rm -rf repo-z repo-mixed
ostree_repo_init repo-z --mode=archive
${CMD_PREFIX} ostree --repo=repo-z commit -b edge --tree=dir=edge >/dev/null
ostree_repo_init repo-mixed --mode=archive-zstd
${CMD_PREFIX} ostree --repo=repo-mixed commit -b edge --tree=dir=edge >/dev/null
zobj=$(ostree_file_path_to_relative_object_path repo-z edge /big)
zstobj=$(ostree_file_path_to_relative_object_path repo-mixed edge /big)
cp repo-z/${zobj} repo-mixed/${zstobj}
if ${CMD_PREFIX} ostree --repo=repo-mixed fsck -q 2>err.txt; then
    assert_not_reached "fsck passed on a zlib payload named .filezst"
fi
echo "ok zlib payload in a .filezst is rejected"

# Prune removes .filezst objects
rm -rf repo-prune
ostree_repo_init repo-prune --mode=archive-zstd
${CMD_PREFIX} ostree --repo=repo-prune commit -b edge --tree=dir=edge >/dev/null
${CMD_PREFIX} ostree --repo=repo-prune refs --delete edge
${CMD_PREFIX} ostree --repo=repo-prune prune --refs-only >/dev/null
find repo-prune/objects -name '*.filezst' > pruned.txt
assert_not_file_has_content pruned.txt .
echo "ok prune"
