#!/usr/bin/env bash
# Usage: static-experiment/build.sh <libostree|consumer> [extra nix build args...]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
what=$1; shift
case $what in
  libostree) out=result-lib ;;
  consumer) out=result-rs ;;
  *) echo "unknown target $what" >&2; exit 2 ;;
esac
cd "$here"
nix build --impure -f . "$what^*" -o "$out" -L "$@" > "build-$what.log" 2>&1 && rc=0 || rc=$?
echo "exit=$rc"
grep -E '^error' "build-$what.log" | head || true
tail -n 30 "build-$what.log" | cut -c1-250
exit $rc
