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

echo '1..5'

# Canonical permissions so that the bare-user-only pull below is lossless
setup_fake_remote_repo1 "archive" "--canonical-permissions"
z_rev=$(${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo rev-parse main)

count_objects() {
    find "$1/objects" -name "*.$2" | wc -l
}

# archive (zlib) remote -> archive-zstd mirror: must recompress
cd ${test_tmpdir}
ostree_repo_init zstd-mirror --mode=archive-zstd
${CMD_PREFIX} ostree --repo=zstd-mirror remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=zstd-mirror pull --mirror origin main
${CMD_PREFIX} ostree --repo=zstd-mirror fsck -q
assert_streq "$(${CMD_PREFIX} ostree --repo=zstd-mirror rev-parse main)" "${z_rev}"
test "$(count_objects zstd-mirror filez)" -eq 0
test "$(count_objects zstd-mirror filezst)" -gt 0
echo "ok mirror archive -> archive-zstd recompresses"

# Same, with --http-trusted, which enables the direct-copy fast path
# when (and only when) the formats match
ostree_repo_init zstd-mirror-trusted --mode=archive-zstd
${CMD_PREFIX} ostree --repo=zstd-mirror-trusted remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=zstd-mirror-trusted pull --mirror --http-trusted origin main
${CMD_PREFIX} ostree --repo=zstd-mirror-trusted fsck -q
test "$(count_objects zstd-mirror-trusted filez)" -eq 0
echo "ok trusted mirror archive -> archive-zstd still recompresses"

# Serve the zstd mirror and go the other way
rm -rf ostree-srv/zstdrepo
cp -a zstd-mirror ostree-srv/zstdrepo
${CMD_PREFIX} ostree --repo=ostree-srv/zstdrepo summary -u

# archive-zstd remote -> archive-zstd mirror with --http-trusted: direct copy
ostree_repo_init zstd-mirror2 --mode=archive-zstd
${CMD_PREFIX} ostree --repo=zstd-mirror2 remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/zstdrepo
${CMD_PREFIX} ostree --repo=zstd-mirror2 pull --mirror --http-trusted origin main
${CMD_PREFIX} ostree --repo=zstd-mirror2 fsck -q
assert_streq "$(${CMD_PREFIX} ostree --repo=zstd-mirror2 rev-parse main)" "${z_rev}"
test "$(count_objects zstd-mirror2 filezst)" -eq "$(count_objects zstd-mirror filezst)"
echo "ok mirror archive-zstd -> archive-zstd"

# archive-zstd remote -> archive (zlib) mirror: must recompress back
ostree_repo_init z-mirror --mode=archive
${CMD_PREFIX} ostree --repo=z-mirror remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/zstdrepo
${CMD_PREFIX} ostree --repo=z-mirror pull --mirror --http-trusted origin main
${CMD_PREFIX} ostree --repo=z-mirror fsck -q
assert_streq "$(${CMD_PREFIX} ostree --repo=z-mirror rev-parse main)" "${z_rev}"
test "$(count_objects z-mirror filezst)" -eq 0
test "$(count_objects z-mirror filez)" -gt 0
echo "ok mirror archive-zstd -> archive recompresses"

# archive-zstd remote -> bare-user-only checkout matches the original content
ostree_repo_init client --mode=bare-user-only
${CMD_PREFIX} ostree --repo=client remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/zstdrepo
${CMD_PREFIX} ostree --repo=client pull origin main
${CMD_PREFIX} ostree --repo=client fsck -q
${CMD_PREFIX} ostree --repo=client checkout -U main client-co
assert_file_has_content client-co/baz/cow '^moo$'
echo "ok pull archive-zstd -> bare-user-only"
