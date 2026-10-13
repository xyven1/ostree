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

# Static deltas using HDiffPatch: the hdiff per-file delta and zstd part
# compression for the classic scheme, and stream deltas.

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_user_xattrs
skip_without_ostree_feature hdiffpatch
skip_without_ostree_feature zstd

echo '1..12'

# Deterministic pseudo-random data: compressible, but not trivially so
gen_data() {
    local seed=$1 kbytes=$2
    python3 -c "
import random, sys
r = random.Random($seed)
words = [bytes(r.randrange(256) for _ in range(r.randrange(3, 12))) for _ in range(500)]
out = bytearray()
while len(out) < $kbytes * 1024:
    out += r.choice(words)
sys.stdout.buffer.write(out[:$kbytes * 1024])
"
}

# Version 1 of the tree
mkdir -p files/bin files/lib/1111-pkg-1.0 files/etc files/share/doc
gen_data 1 600 > files/bin/tool
gen_data 2 300 > files/bin/other
gen_data 3 900 > files/lib/1111-pkg-1.0/libbig.so
gen_data 4 20 > files/lib/1111-pkg-1.0/libsmall.so
gen_data 5 40 > files/etc/secret
chmod 600 files/etc/secret
echo "unchanged" > files/etc/unchanged
: > files/etc/empty
ln -s tool files/bin/tool-link
for i in $(seq 1 20); do echo "doc $i" > files/share/doc/doc$i; done
${CMD_PREFIX} ostree --repo=repo init --mode=archive
${CMD_PREFIX} ostree --repo=repo commit -b test -s v1 --tree=dir=files
origrev=$(${CMD_PREFIX} ostree --repo=repo rev-parse test)

# Version 2: small edits to big files, a directory rename (as with
# hash-prefixed package directories), additions and removals.
(cat files/bin/tool; echo "appended") > files/bin/tool.new
mv files/bin/tool.new files/bin/tool
printf 'patched' | dd of=files/bin/other bs=1 seek=1000 conv=notrunc status=none
mv files/lib/1111-pkg-1.0 files/lib/2222-pkg-1.0
printf 'x' | dd of=files/lib/2222-pkg-1.0/libbig.so bs=1 seek=500000 conv=notrunc status=none
printf 'y' | dd of=files/etc/secret bs=1 seek=100 conv=notrunc status=none
gen_data 6 20 > files/bin/brandnew
: > files/etc/empty2
rm files/share/doc/doc3 files/share/doc/doc4
echo "new doc" > files/share/doc/doc-new
ln -sf other files/bin/tool-link
ln -s ../lib files/bin/lib-link
${CMD_PREFIX} ostree --repo=repo commit -b test -s v2 --tree=dir=files
newrev=$(${CMD_PREFIX} ostree --repo=repo rev-parse test)

# A bare-user copy to generate from too (no unpacking needed there)
${CMD_PREFIX} ostree --repo=repo-bare init --mode=bare-user
${CMD_PREFIX} ostree --repo=repo-bare pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=repo-bare pull-local repo ${newrev}

${CMD_PREFIX} ostree --repo=repo ls -R -C ${newrev} > expected-ls.txt

# apply_and_check DELTAFILE [from-empty]: apply a delta file to a fresh
# bare-user repo holding only the origin commit, and verify the result.
apply_and_check() {
    local delta=$1 empty=${2:-}
    rm -rf target
    ${CMD_PREFIX} ostree --repo=target init --mode=bare-user
    if test -z "${empty}"; then
        ${CMD_PREFIX} ostree --repo=target pull-local repo ${origrev}
    fi
    ${CMD_PREFIX} ostree --repo=target static-delta apply-offline ${delta}
    ${CMD_PREFIX} ostree --repo=target fsck
    ${CMD_PREFIX} ostree --repo=target ls -R -C ${newrev} > actual-ls.txt
    assert_files_equal expected-ls.txt actual-ls.txt
}

mkdir -p deltas/hdiff-dir deltas/zstd-dir deltas/stream-dir

# The classic scheme, for size comparisons below
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --min-fallback-size=0 --inline --filename=deltas/classic
apply_and_check deltas/classic
classic_size=$(stat -c '%s' deltas/classic)
echo "ok classic baseline"

# hdiff as the per-file delta
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --min-fallback-size=0 --filename=deltas/hdiff-dir/superblock --file-delta=hdiff \
              2>&1 | tee out.txt
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --min-fallback-size=0 --file-delta=hdiff
${CMD_PREFIX} ostree --repo=repo static-delta show ${origrev}-${newrev} > show.txt
assert_file_has_content show.txt 'hpatch=[1-9]'
assert_file_has_content show.txt 'bspatch=0'
apply_and_check deltas/hdiff-dir/superblock
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --min-fallback-size=0 --inline --filename=deltas/hdiff-inline --file-delta=hdiff
apply_and_check deltas/hdiff-inline
if ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
                 --file-delta=nope --filename=deltas/bad 2>err.txt; then
    assert_not_reached "invalid --file-delta unexpectedly succeeded"
fi
assert_file_has_content err.txt "Invalid 'file-delta'"
echo "ok hdiff per-file delta"

# zstd part compression, with and without hdiff
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --min-fallback-size=0 --inline --filename=deltas/zstd --compression=zstd
apply_and_check deltas/zstd
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --min-fallback-size=0 --inline --filename=deltas/zstd-hdiff --compression=zstd:19 \
              --file-delta=hdiff
apply_and_check deltas/zstd-hdiff
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --min-fallback-size=0 --filename=deltas/zstd-dir/superblock --compression=zstd:1
apply_and_check deltas/zstd-dir/superblock
for bad in lz4 zstd:abc zstd:99 xz:3; do
    if ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
                     --compression=${bad} --filename=deltas/bad 2>err.txt; then
        assert_not_reached "--compression=${bad} unexpectedly succeeded"
    fi
done
echo "ok zstd part compression"

# Stream deltas at every effort, inline
for effort in 0 1 2 3 4 5 6 7 8 9; do
    ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
                  --inline --filename=deltas/stream-${effort} --stream --effort=${effort}
    apply_and_check deltas/stream-${effort}
done
echo "ok stream delta at every effort"

# The whole point: far smaller than the classic scheme on an update
stream_size=$(stat -c '%s' deltas/stream-5)
if test $((stream_size * 3)) -gt ${classic_size}; then
    assert_not_reached "stream delta (${stream_size}) not much smaller than classic (${classic_size})"
fi
echo "ok stream delta size"

# Generated from a bare-user repo, parts as separate files, single threaded
${CMD_PREFIX} ostree --repo=repo-bare static-delta generate --from=${origrev} --to=${newrev} \
              --filename=deltas/stream-dir/superblock --stream --threads=1 2>&1 | tee out.txt
assert_file_has_content out.txt 'stream part 1 (modified)'
assert_has_file deltas/stream-dir/0
apply_and_check deltas/stream-dir/superblock
echo "ok stream delta from bare-user repo with separate parts"

# From empty
for effort in 0 5 9; do
    ${CMD_PREFIX} ostree --repo=repo static-delta generate --empty --to=${newrev} \
                  --inline --filename=deltas/stream-empty-${effort} --stream --effort=${effort} \
                  2>&1 | tee out.txt
    assert_file_has_content out.txt 'stream part 1 (all)'
    apply_and_check deltas/stream-empty-${effort} empty
done
${CMD_PREFIX} ostree --repo=repo-bare static-delta generate --empty --to=${newrev} \
              --inline --filename=deltas/stream-empty-bare --stream
apply_and_check deltas/stream-empty-bare empty
echo "ok stream delta from empty"

# Objects the target already has are skipped, not rewritten
rm -rf target
${CMD_PREFIX} ostree --repo=target init --mode=bare-user
${CMD_PREFIX} ostree --repo=target pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/stream-5
${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/stream-5
${CMD_PREFIX} ostree --repo=target fsck
echo "ok stream delta applied twice"

# In-repository stream delta: show, and pull through it
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --stream
${CMD_PREFIX} ostree --repo=repo static-delta show ${origrev}-${newrev} > show.txt
assert_file_has_content show.txt 'PartPayload0: stream kind=hdiff'
${CMD_PREFIX} ostree --repo=repo summary -u
rm -rf target
${CMD_PREFIX} ostree --repo=target init --mode=bare-user
${CMD_PREFIX} ostree --repo=target pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=target pull-local --require-static-deltas repo ${newrev}
${CMD_PREFIX} ostree --repo=target fsck
${CMD_PREFIX} ostree --repo=target ls -R -C ${newrev} > actual-ls.txt
assert_files_equal expected-ls.txt actual-ls.txt
echo "ok pull via stream delta"

# Applying needs the origin objects, and a bare repository
rm -rf target
${CMD_PREFIX} ostree --repo=target init --mode=bare-user
if ${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/stream-5 2>err.txt; then
    assert_not_reached "applying a stream delta without its origin unexpectedly succeeded"
fi
rm -rf target
${CMD_PREFIX} ostree --repo=target init --mode=archive
${CMD_PREFIX} ostree --repo=target pull-local repo ${origrev}
if ${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/stream-5 2>err.txt; then
    assert_not_reached "applying a stream delta to an archive repo unexpectedly succeeded"
fi
assert_file_has_content err.txt 'only be applied to bare repositories'
echo "ok stream delta apply preconditions"

# Damaged deltas fail cleanly, without writing bad objects.  Separate part
# files are checksummed; inline parts are not, so there the damage reaches
# the decompressor, patcher and object checksums.
fresh_target() {
    rm -rf target
    ${CMD_PREFIX} ostree --repo=target init --mode=bare-user
    ${CMD_PREFIX} ostree --repo=target pull-local repo ${origrev}
}
size=$(stat -c '%s' deltas/stream-dir/0)
rm -rf damaged && cp -r deltas/stream-dir damaged
truncate -s $((size / 2)) damaged/0
fresh_target
if ${CMD_PREFIX} ostree --repo=target static-delta apply-offline damaged/superblock 2>err.txt; then
    assert_not_reached "applying a truncated stream delta unexpectedly succeeded"
fi
assert_file_has_content err.txt 'Checksum mismatch in static delta part'
for effort in 0 5; do
    size=$(stat -c '%s' deltas/stream-${effort})
    for offset in 40 400 2000; do
        cp deltas/stream-${effort} damaged-inline
        printf '\377\000\377\000\377\000\377\000' \
            | dd of=damaged-inline bs=1 seek=$((size - offset)) conv=notrunc status=none
        fresh_target
        if ${CMD_PREFIX} ostree --repo=target static-delta apply-offline damaged-inline 2>err.txt; then
            # The damage may land where it does not matter; then the result must be right
            ${CMD_PREFIX} ostree --repo=target ls -R -C ${newrev} > actual-ls.txt
            assert_files_equal expected-ls.txt actual-ls.txt
        fi
        ${CMD_PREFIX} ostree --repo=target fsck
    done
done
echo "ok damaged stream delta"

# Option validation
if ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
                 --stream --effort=10 --filename=deltas/bad 2>err.txt; then
    assert_not_reached "--effort=10 unexpectedly succeeded"
fi
assert_file_has_content err.txt "Invalid 'effort'"
if ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
                 --effort=3 --filename=deltas/bad 2>err.txt; then
    assert_not_reached "--effort without --stream unexpectedly succeeded"
fi
assert_file_has_content err.txt "require --stream"
echo "ok option validation"
