# HDiffPatch-based static deltas

Status: implemented on branch `worktree-hdiff-delta`. Written after the fact from four rounds of
measurement on real operating system repositories; it records what was built, why, and the decisions that are
still open to being overruled.

## Goal

Static delta generation is too slow, and the deltas are larger than they need to be, for the one
configuration that matters here: a single self-contained file
(`static-delta generate --min-fallback-size=0 --inline`), applied offline to a bare repository.

Two things were asked for:

1. A **drop-in**: keep ostree's delta scheme exactly, but replace bsdiff with HDiffPatch and xz
   with zstd.
2. A **new pipeline** that diffs whole groups of objects at once, with one setting that goes from
   "as fast as possible" (development loops) to "as small as possible" (updates over expensive links).

## What the measurements showed

- On real updates bsdiff is about 95% of generation time. On from-empty deltas xz is about 100%.
- HDiffPatch is roughly 100 times faster than bsdiff on the same file pairs.
- Swapping the per-file algorithm barely changes size. What ostree ships undiffed dominates a small
  update: new `dirtree` objects, rollsum-matched files, files without a partner.
- HDiffPatch's two modes suit opposite cases. The precise in-memory differ is fast only on similar
  data (its block pre-match skips identical regions at over 1 GB/s) and gives diffs several times
  smaller there. The block-matching stream differ is fast on anything but poor on similar data.
- Pairing by path fails on this data: builds contain hash-prefixed directories, so a rebuilt binary
  is a delete plus an add. Pairing by basename against *deleted* files works.

## Design

### Shared foundation

- HDiffPatch v5.1.3 is a git submodule (`hdiffpatch/`, MIT), like bsdiff and libglnx. Only the
  subset needed for uncompressed single-stream diffs (`HDIFFSF20`) is built, as a private
  convenience library, behind `--with-hdiffpatch` (auto-detected). libostree gains a libstdc++
  dependency when it is enabled; no symbols are exported.
- `ostree-hdiff.{h,cpp}` is a small C API over it: precise differ, stream differ, in-memory and
  streaming patchers. HDiffPatch never compresses; ostree does that itself.
- zstd support comes from the `--with-zstd` commit shared with the archive-zstd branch (cherry-picked
  unchanged, so the two branches merge cleanly).

### Drop-in

- `--file-delta=hdiff`: per-file deltas use the precise differ and are emitted as a new `HPATCH`
  opcode next to `BSPATCH`. Not subject to `--max-bsdiff-size`.
- `--compression=zstd[:LEVEL]`: parts carry compression byte `z`.
- Defaults are unchanged. Rollsum, object pairing and part layout are untouched.

### Stream deltas (`--stream`)

- Objects of a part are concatenated into a *new stream*: metadata objects first, then file content,
  each ordered by path. The part holds that stream, or one diff producing it from an *old stream*
  made of objects the client already has.
- Up to two parts, generated concurrently:
  - **modified**: new metadata, plus new files whose basename matches a file gone from the target;
    diffed precisely against the gone metadata and files.
  - **new**: every other new file; block-matched against old files sharing a basename.
- From-empty: one part, the compressed stream, no differ involved.
- The superblock is unchanged, so inline parts, signing, `pull` and `apply-offline` work as before.
  A stream part is recognised by a leading `s` where other parts have their compression byte. The
  format is specified in `docs/formats.md`.
- Applying streams end to end: old data is read by random access from repository objects, the diff
  is decompressed as it is consumed, objects are written and checksummed as they are produced.
  Measured memory while applying is about 15 MB plus the mapped delta file.

### Effort

`--effort=N`, default 5. Compression is multi-threaded (`--threads`, default one per processor).

| Effort | Compression | Notes |
|---|---|---|
| 0 | zstd 1 | coarser block matching |
| 1 | zstd 3 | |
| 2 | zstd 6 | |
| 3 | zstd 10, 16 MiB window | |
| 4 | zstd 12, 32 MiB window, long-distance matching | |
| 5 | xz 1, 8 MiB dictionary | default; about the size of today's deltas |
| 6 | xz 6, 8 MiB dictionary | |
| 7 | xz 6, 32 MiB dictionary | |
| 8 | xz 9, 64 MiB dictionary | |
| 9 | xz 9 extreme, 64 MiB dictionary, single block, single thread | larger pre-match blocks in the precise differ |

Diffs of 4 MiB or less are always compressed at the strongest setting of the effort's codec, since
the time is negligible. That is why effort barely matters for an ordinary update.

## Results

Measured with the implementation on a 32-core machine; every row was applied into a fresh repository
and passed `ostree fsck`. "Classic" is `--min-fallback-size=0 --inline` on the same commits.

### Updates

| Pair | Classic | Drop-in hdiff + zstd | Stream, effort 5 |
|---|--:|--:|--:|
| SDK OS rebuild | 75 KB, 23.7 s | 72 KB, 2.4 s | 18 KB, 0.6 s |
| SDK software rebuild | 338 KB, 1.1 s | 313 KB, 0.3 s | 33 KB, 0.1 s |
| Software tarball v1 to v2 (758 MB changed) | 15.4 MB, 201 s | 432 KB, 7.3 s | 128 KB, 3.0 s |
| Recovery rebuild | 121 KB, 24.4 s | 118 KB, 2.5 s | 41 KB, 0.9 s |

### Mostly new content

| Pair | Classic | Effort 0 | Effort 3 | Effort 5 | Effort 6 | Effort 8 |
|---|--:|--:|--:|--:|--:|--:|
| base to recovery (265 MB new) | 35.9 MB, 96 s | 53.9 MB, 1.2 s | 37.7 MB, 2.8 s | 33.8 MB, 3.7 s | 29.9 MB, 9.8 s | 29.0 MB, 59 s |
| base to sysroot (1.42 GB new) | 269.5 MB, 504 s | 407 MB, 4.1 s | 312 MB, 8.4 s | 283 MB, 11.8 s | 251 MB, 27 s | 224 MB, 90 s |

Effort 9 gives 29.1 MB in 68 s and 220 MB in 536 s on these two pairs: on the larger one, 18%
smaller than classic in about the same time. Efforts 8 and 9 are within 2% of each other in size;
9 mainly removes the cost of splitting xz into blocks, and is single-threaded.

### From empty

| Commit | Classic | Effort 0 | Effort 3 | Effort 5 | Effort 6 |
|---|--:|--:|--:|--:|--:|
| SDK software (321 MB) | 64.4 MB, 89 s | 100 MB, 0.7 s | 76.5 MB, 2.0 s | 69.2 MB, 3.4 s | n/a |
| SDK OS (2.84 GB) | 718 MB, 1115 s | 1009 MB, 5.8 s | 794 MB, 13.7 s | 730 MB, 25 s | 654 MB, 52 s |

For the SDK software commit, effort 7 gives 56.9 MB in 32 s and effort 9 gives 53.9 MB in 125 s.
The classic figure for the SDK OS was measured while other jobs were running and is about 25% high.

## Decisions that are open to being overruled

- **Non-world-readable diff sources.** The classic scheme refuses to use a file as a delta source
  unless it is world-readable, so that an unprivileged client with a root-owned parent repository
  can still apply the delta. Stream deltas use such files. On the SDK OS pair, honouring the rule
  made the delta 47 KB instead of 18 KB, because the files that change every build (package caches,
  logs) are exactly the unreadable ones. This is safe for devices that apply updates as root; it is
  a one-line change in `generator_load_old_file()` to go back.
- **Default effort 5.** Chosen because it roughly matches the size of today's deltas at 30 to 100
  times the speed. Efforts 0 to 4 need zstd on the client.
- **No fallback objects in stream deltas.** `--min-fallback-size` is ignored rather than rejected.
- **Compatibility.** All three features produce deltas that an unmodified ostree rejects with a
  clear error. Nothing changes unless the new options are passed.
- **Pure suffix-array matching is not offered.** HDiffPatch's `-block-0` mode found no matches when
  driven through its library entry points in this build, for reasons not yet understood (it is not
  memory corruption: ASan and UBSan are clean, and the result is a valid but useless diff). Effort 9
  uses 64 KiB pre-match blocks instead, which measured within about 4% of that mode's size.

## Known limits

- Stream deltas apply only to bare repositories (the same restriction as the classic `open`
  instruction), and need hdiffpatch on the client.
- The precise differ holds the modified group in memory; above 4 GiB of old plus new data it falls
  back to the block-matching differ.
- High efforts with many threads use several GiB while generating (xz keeps a block per thread).
  `--threads` bounds it.
- Generating from a non-bare repository first unpacks the file content it needs into a temporary
  file, and does not run the two parts concurrently.
- Not yet measured: a true consecutive sysroot release pair, and apply time on the arm64 target.

## Testing

- `tests/test-hdiff.c`: the wrapper (round trips through both differs and both patchers, empty
  source, rejection of mismatched sizes and corrupt diffs).
- `tests/test-delta-hdiff.sh`: the drop-in options, and stream deltas at every effort; inline and
  separate parts; generated from archive and bare-user repositories; from a commit and from empty;
  applied offline and through `pull --require-static-deltas`; apply preconditions; damaged deltas;
  option validation.
