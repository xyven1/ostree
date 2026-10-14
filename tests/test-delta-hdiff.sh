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

# Bulk static deltas (HDiffPatch over whole groups of objects).

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_user_xattrs
skip_without_ostree_feature hdiffpatch
skip_without_ostree_feature zstd

echo '1..13'

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

mkdir -p deltas/bulk-dir

# The classic scheme, for size comparisons below
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --min-fallback-size=0 --inline --filename=deltas/classic
apply_and_check deltas/classic
classic_size=$(stat -c '%s' deltas/classic)
echo "ok classic baseline"

# Bulk deltas at every effort, inline
for effort in 0 1 2 3 4 5 6 7 8 9; do
    ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
                  --inline --filename=deltas/bulk-${effort} --bulk --effort=${effort}
    apply_and_check deltas/bulk-${effort}
done
echo "ok bulk delta at every effort"

# The whole point: far smaller than the classic scheme on an update, even
# though by default neither diffs the non-world-readable file.
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --inline --filename=deltas/size-bulk --bulk
bulk_size=$(stat -c '%s' deltas/size-bulk)
if test ${bulk_size} -ge ${classic_size}; then
    assert_not_reached "bulk delta (${bulk_size}) not smaller than classic (${classic_size})"
fi
# With everything diffable it is at least a third smaller on this tree
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --inline --filename=deltas/size-bulk --bulk --allow-private-sources
bulk_size=$(stat -c '%s' deltas/size-bulk)
if test $((bulk_size * 3)) -gt $((classic_size * 2)); then
    assert_not_reached "bulk delta (${bulk_size}) not much smaller than classic (${classic_size})"
fi
echo "ok bulk delta size"

# Generated from a bare-user repo, parts as separate files, single threaded
${CMD_PREFIX} ostree --repo=repo-bare static-delta generate --from=${origrev} --to=${newrev} \
              --filename=deltas/bulk-dir/superblock --bulk --threads=1 2>&1 | tee out.txt
assert_file_has_content out.txt 'bulk part 1 (modified)'
assert_has_file deltas/bulk-dir/0
apply_and_check deltas/bulk-dir/superblock
echo "ok bulk delta from bare-user repo with separate parts"

# From empty
for effort in 0 5 9; do
    ${CMD_PREFIX} ostree --repo=repo static-delta generate --empty --to=${newrev} \
                  --inline --filename=deltas/bulk-empty-${effort} --bulk --effort=${effort} \
                  2>&1 | tee out.txt
    assert_file_has_content out.txt 'bulk part 1 (all)'
    apply_and_check deltas/bulk-empty-${effort} empty
done
${CMD_PREFIX} ostree --repo=repo-bare static-delta generate --empty --to=${newrev} \
              --inline --filename=deltas/bulk-empty-bare --bulk
apply_and_check deltas/bulk-empty-bare empty
echo "ok bulk delta from empty"

# Objects the target already has are skipped, not rewritten
rm -rf target
${CMD_PREFIX} ostree --repo=target init --mode=bare-user
${CMD_PREFIX} ostree --repo=target pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/bulk-5
${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/bulk-5
${CMD_PREFIX} ostree --repo=target fsck
echo "ok bulk delta applied twice"

# In-repository bulk delta: show, and pull through it
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} --bulk
${CMD_PREFIX} ostree --repo=repo static-delta show ${origrev}-${newrev} > show.txt
assert_file_has_content show.txt 'PartPayload0: bulk kind=hdiff'
${CMD_PREFIX} ostree --repo=repo summary -u
rm -rf target
${CMD_PREFIX} ostree --repo=target init --mode=bare-user
${CMD_PREFIX} ostree --repo=target pull-local repo ${origrev}
${CMD_PREFIX} ostree --repo=target pull-local --require-static-deltas repo ${newrev}
${CMD_PREFIX} ostree --repo=target fsck
${CMD_PREFIX} ostree --repo=target ls -R -C ${newrev} > actual-ls.txt
assert_files_equal expected-ls.txt actual-ls.txt
echo "ok pull via bulk delta"

# Applying needs the origin objects, and a bare repository
rm -rf target
${CMD_PREFIX} ostree --repo=target init --mode=bare-user
if ${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/bulk-5 2>err.txt; then
    assert_not_reached "applying a bulk delta without its origin unexpectedly succeeded"
fi
rm -rf target
${CMD_PREFIX} ostree --repo=target init --mode=archive
${CMD_PREFIX} ostree --repo=target pull-local repo ${origrev}
if ${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/bulk-5 2>err.txt; then
    assert_not_reached "applying a bulk delta to an archive repo unexpectedly succeeded"
fi
assert_file_has_content err.txt 'only be applied to bare repositories'
echo "ok bulk delta apply preconditions"

# Damaged deltas fail cleanly, without writing bad objects.  Separate part
# files are checksummed; inline parts are not, so there the damage reaches
# the decompressor, patcher and object checksums.
fresh_target() {
    rm -rf target
    ${CMD_PREFIX} ostree --repo=target init --mode=bare-user
    ${CMD_PREFIX} ostree --repo=target pull-local repo ${origrev}
}
size=$(stat -c '%s' deltas/bulk-dir/0)
rm -rf damaged && cp -r deltas/bulk-dir damaged
truncate -s $((size / 2)) damaged/0
fresh_target
if ${CMD_PREFIX} ostree --repo=target static-delta apply-offline damaged/superblock 2>err.txt; then
    assert_not_reached "applying a truncated bulk delta unexpectedly succeeded"
fi
assert_file_has_content err.txt 'Checksum mismatch in static delta part'
for effort in 0 5; do
    size=$(stat -c '%s' deltas/bulk-${effort})
    for offset in 40 400 2000; do
        cp deltas/bulk-${effort} damaged-inline
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
echo "ok damaged bulk delta"

# Files that are not world readable (etc/secret here) are only used as delta
# sources on request
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --inline --filename=deltas/private-off --bulk
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
              --inline --filename=deltas/private-on --bulk --allow-private-sources
apply_and_check deltas/private-off
apply_and_check deltas/private-on
off_size=$(stat -c '%s' deltas/private-off)
on_size=$(stat -c '%s' deltas/private-on)
# The 40k secret file differs by one byte; shipping it whole costs kilobytes
if test $((on_size + 4096)) -gt ${off_size}; then
    assert_not_reached "private sources did not shrink the delta (${on_size} vs ${off_size})"
fi
echo "ok private delta sources"

# Parts larger than a few megabytes are read, compressed and written by
# separate threads when more than one is allowed; the result must apply the
# same.  This needs more data than the tree above has.
mkdir -p bigfiles/sub
head -c 7000000 /dev/urandom > bigfiles/random
gen_data 8 3000 > bigfiles/sub/compressible
cp files/bin/tool bigfiles/sub/tool
${CMD_PREFIX} ostree --repo=repo commit -b big -s big --tree=dir=bigfiles
bigrev=$(${CMD_PREFIX} ostree --repo=repo rev-parse big)
${CMD_PREFIX} ostree --repo=repo ls -R -C ${bigrev} > expected-big-ls.txt
for effort in 0 3 5; do
    for threads in 1 4; do
        ${CMD_PREFIX} ostree --repo=repo static-delta generate --empty --to=${bigrev} --inline \
                      --filename=deltas/big-${effort}-${threads} --bulk --effort=${effort} --threads=${threads}
        rm -rf target
        ${CMD_PREFIX} ostree --repo=target init --mode=bare-user
        ${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/big-${effort}-${threads}
        ${CMD_PREFIX} ostree --repo=target fsck
        ${CMD_PREFIX} ostree --repo=target ls -R -C ${bigrev} > actual-big-ls.txt
        assert_files_equal expected-big-ls.txt actual-big-ls.txt
    done
done
echo "ok bulk delta with pipelined part compression"

# A file whose name changes only in its version number is still treated as a
# new version of the old file, and goes to the "modified" part along with the
# metadata; an unrelated new file does not.
mkdir -p verfiles/lib
head -c 200000 /dev/urandom > verfiles/lib/libver.so.1.2.3
${CMD_PREFIX} ostree --repo=repo commit -b ver -s v1 --tree=dir=verfiles
mv verfiles/lib/libver.so.1.2.3 verfiles/lib/libver.so.1.10.0
echo changed >> verfiles/lib/libver.so.1.10.0
head -c 300000 /dev/urandom > verfiles/lib/unrelated
${CMD_PREFIX} ostree --repo=repo commit -b ver -s v2 --tree=dir=verfiles
verfrom=$(${CMD_PREFIX} ostree --repo=repo rev-parse ver^)
verto=$(${CMD_PREFIX} ostree --repo=repo rev-parse ver)
${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${verfrom} --to=${verto} --inline \
              --filename=deltas/ver --bulk 2>&1 | tee out.txt
assert_file_has_content out.txt 'bulk part 1 (modified): objects=[0-9]* sources=[0-9]* new=2[0-9][0-9][0-9][0-9][0-9] '
assert_file_has_content out.txt 'bulk part 2 (new): objects=1 '
rm -rf target
${CMD_PREFIX} ostree --repo=target init --mode=bare-user
${CMD_PREFIX} ostree --repo=target pull-local repo ${verfrom}
${CMD_PREFIX} ostree --repo=target static-delta apply-offline deltas/ver
${CMD_PREFIX} ostree --repo=target fsck
${CMD_PREFIX} ostree --repo=repo ls -R -C ${verto} > expected-ver-ls.txt
${CMD_PREFIX} ostree --repo=target ls -R -C ${verto} > actual-ver-ls.txt
assert_files_equal expected-ver-ls.txt actual-ver-ls.txt
echo "ok bulk delta pairs files renamed by version"

# Option validation
if ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
                 --bulk --effort=10 --filename=deltas/bad 2>err.txt; then
    assert_not_reached "--effort=10 unexpectedly succeeded"
fi
assert_file_has_content err.txt "Invalid 'effort'"
if ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
                 --effort=3 --filename=deltas/bad 2>err.txt; then
    assert_not_reached "--effort without --bulk unexpectedly succeeded"
fi
assert_file_has_content err.txt "requires --bulk"
for opt in --threads=2 --allow-private-sources; do
    if ${CMD_PREFIX} ostree --repo=repo static-delta generate --from=${origrev} --to=${newrev} \
                     ${opt} --filename=deltas/bad 2>err.txt; then
        assert_not_reached "${opt} without --bulk unexpectedly succeeded"
    fi
    assert_file_has_content err.txt "requires --bulk"
done
echo "ok option validation"
