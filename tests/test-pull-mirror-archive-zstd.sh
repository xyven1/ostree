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

echo '1..7'

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

# Without a summary, the remote's mode comes from its config file.  If that
# can't be understood the pull must say so, rather than guess the format and
# fail later with 404s for content objects.
cp -a ostree-srv/zstdrepo ostree-srv/badcfgrepo
rm -f ostree-srv/badcfgrepo/summary ostree-srv/badcfgrepo/summary.sig
echo 'this is not a keyfile' > ostree-srv/badcfgrepo/config
ostree_repo_init client-badcfg --mode=bare-user-only
${CMD_PREFIX} ostree --repo=client-badcfg remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/badcfgrepo
if ${CMD_PREFIX} ostree --repo=client-badcfg pull origin main 2>err.txt; then
    assert_not_reached "pull succeeded with an unparsable remote config"
fi
assert_file_has_content err.txt 'Determining remote repository mode'
assert_not_file_has_content err.txt '404'
echo "ok unparsable remote config is a clear error"

# Compatibility: a zlib archive remote with neither summary nor config file
# still works, as it did before archive-zstd existed
cp -a ostree-srv/gnomerepo ostree-srv/nocfgrepo
rm -f ostree-srv/nocfgrepo/summary ostree-srv/nocfgrepo/summary.sig ostree-srv/nocfgrepo/config
ostree_repo_init client-nocfg --mode=bare-user-only
${CMD_PREFIX} ostree --repo=client-nocfg remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/nocfgrepo
${CMD_PREFIX} ostree --repo=client-nocfg pull origin main
${CMD_PREFIX} ostree --repo=client-nocfg fsck -q
assert_streq "$(${CMD_PREFIX} ostree --repo=client-nocfg rev-parse main)" "${z_rev}"
echo "ok zlib archive remote without config or summary still pulls"
