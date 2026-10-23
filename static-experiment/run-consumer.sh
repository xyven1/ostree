#!/usr/bin/env bash
# Run the static Rust consumer: once pulling over file://, once over http://
# (exercising the statically linked curl fetcher), each with an empty env.
set -euo pipefail
cd "$(dirname "$0")"
bin=$(readlink -f result-rs)/bin/ostree-static-consumer
work=$(mktemp -d "${TMPDIR:-/tmp}/ostree-static.XXXXXX")
srv=""
trap 'kill $srv 2>/dev/null || true; rm -rf "$work"' EXIT

echo "== binary"
file -b "$bin" | cut -c1-100
ls -la "$bin" | awk '{print $5, "bytes"}'
{ ldd "$bin" 2>&1 || true; } | sed "s/^/  ldd: /"
echo "== runtime closure (nix store paths referenced)"
nix-store -qR "$(readlink -f result-rs)" | sed 's/^/  /'

echo "== file:// pull"
env -i "$bin" "$work/a"

echo "== http:// pull"
mkdir -p "$work/b"
# Pre-seed src-repo by running once, then serve it and pull from a fresh dir.
env -i "$bin" "$work/seed" > /dev/null
python3 -m http.server -d "$work/seed/src-repo" -b 127.0.0.1 18765 > "$work/http.log" 2>&1 &
srv=$!
sleep 1
env -i "$bin" "$work/b" http://127.0.0.1:18765 || { cat "$work/http.log"; exit 1; }
