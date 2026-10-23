#!/usr/bin/env bash
# Show what the static libostree build produced.
set -euo pipefail
cd "$(dirname "$0")"
lib=$(readlink -f result-lib)
dev=$(readlink -f result-lib-dev)
echo "== shared objects (expect none)"
find "$lib" "$dev" -name '*.so*' | sed 's/^/  /'
echo "== archives"
find "$lib" "$dev" -name '*.a' -exec ls -la {} + | sed 's/^/  /'
echo "== binaries"
for b in "$lib"/bin/* "$lib"/libexec/libostree/*; do
  [ -f "$b" ] && printf '  %s: %s\n' "${b#$lib/}" "$(file -b "$b" | cut -c1-90)"
done
echo "== ostree --version"
"$lib/bin/ostree" --version | sed 's/^/  /'
echo "== ostree-1.pc"
sed 's/^/  /' "$dev/lib/pkgconfig/ostree-1.pc"
