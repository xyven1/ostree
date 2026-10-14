#!/bin/bash
# Build the pre-optimization commit (99dd39d) in a scratch directory for A/B runs.
# Run from the worktree root inside the dev shell.
set -euo pipefail
old=/var/tmp/s3-bench/old-src
rm -rf "${old}"
mkdir -p "${old}"
git archive 99dd39d | tar -x -C "${old}"
cp -a libglnx/. "${old}/libglnx/"
cp -a bsdiff/. "${old}/bsdiff/"
cd "${old}"
env NOCONFIGURE=1 ./autogen.sh > build.log 2>&1
./configure --with-curl --with-aws >> build.log 2>&1
make -j"$(nproc)" >> build.log 2>&1
./ostree --version | grep -- '- aws'
echo "old build ready: ${old}/ostree"
