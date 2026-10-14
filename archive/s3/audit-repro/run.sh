#!/bin/bash
# Reproduce the PR #2 handoff-audit blockers against a build of the s3-remotes branch.
#   run.sh <ostree build dir> <mode> [valgrind]
# mode: firstempty     -> --http-trusted mirror links a 0-byte object (blocker 2)
#       firstdrop      -> in-process pulls; heap corruption on the 3rd pull (blocker 1)
#       firstnorange | firstshortbody -> fixed in 071a83a, should fail cleanly
# Needs python3; firstdrop builds inproc-pull.c inside the build's nix dev shell.
set -euo pipefail
B=$(realpath "$1"); MODE=$2; HERE=$(dirname "$(realpath "$0")")
W=$(mktemp -d); trap 'kill ${SP:-} 2>/dev/null; rm -rf "$W"' EXIT; cd "$W"
mkdir -p root/bkt src; head -c $((10*1024*1024)) /dev/urandom > src/big; echo small > src/small
"$B/ostree" --repo=root/bkt/repo init --mode=archive >/dev/null
"$B/ostree" --repo=root/bkt/repo commit -b main --tree=dir=src >/dev/null
echo "$MODE" > mode; PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
python3 "$HERE/s3-fault-server-extra.py" "$W/root" "$W/mode" "$W/log" "$PORT" & SP=$!; sleep 1
export AWS_ACCESS_KEY_ID=x AWS_SECRET_ACCESS_KEY=y AWS_EC2_METADATA_DISABLED=true AWS_CONFIG_FILE=/dev/null AWS_SHARED_CREDENTIALS_FILE=/dev/null
if [ "$MODE" = firstdrop ]; then
  (cd "$B" && nix develop '.?shallow=1' -c bash -c "gcc -o $W/inproc $HERE/inproc-pull.c -I src/libostree -I . \$(pkg-config --cflags --libs gio-unix-2.0) -L .libs -lostree-1 -Wl,-rpath,$B/.libs 2>/dev/null")
  if [ "${3:-}" = valgrind ]; then nix shell nixpkgs#valgrind -c valgrind "$W/inproc" "$W" "$PORT" 2; else "$W/inproc" "$W" "$PORT" 3; fi
  exit
fi
"$B/ostree" --repo=m init --mode=archive
"$B/ostree" --repo=m remote add --no-gpg-verify origin "s3://bkt/repo?endpoint=http://127.0.0.1:$PORT"
set +e; "$B/ostree" --repo=m pull --mirror --http-trusted --disable-static-deltas origin main; echo "pull exit=$?"
"$B/ostree" --repo=m fsck; echo "fsck exit=$?"
