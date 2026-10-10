# `archive-zstd` repository mode — design

Date: 2026-10-09
Status: awaiting review

## 1. Goal

Let an ostree archive repository store and serve content objects compressed
with zstd instead of zlib. Today the only compressed mode is `archive`
(`archive-z2`): each content object is a `.filez` holding a small header
followed by a raw deflate stream, compressed single-threaded at level 6.
zstd gives a better ratio at a fraction of the CPU cost, on both the compose
side and the client side.

Success means:

- `ostree init --mode=archive-zstd` creates a working repository.
- `commit`, `checkout`, `fsck`, `prune`, `ls`, `cat`, `export`, `summary`
  and static-delta generation work against it exactly as they do against
  `archive`.
- A client built with zstd can pull from an `archive-zstd` remote over HTTP
  or `file://` into any repository mode, and can mirror it.
- A client without zstd support, or an older client, fails immediately with
  a clear message rather than with 404s or corruption errors mid-pull.
- Existing `archive` repositories and clients are entirely unaffected.

## 2. Prior art

- Upstream `ostreedev/ostree` contains no zstd code, PR or commit. The only
  reference is open issue #2626 (2022), where a maintainer wrote "I think it
  would make sense to introduce `archive-zstd` or so". This design follows
  that suggestion.
- GitHub code search for likely symbol names, and a branch-name scan of the
  150 most recently created forks, found no implementation.

## 3. Non-goals

- zstd for static delta parts (currently lzma, type byte `'x'`). That is a
  separate wire-format change.
- Multi-threaded compression (`ZSTD_c_nbWorkers`), dictionaries, or
  long-distance matching.
- In-place conversion of an existing `archive` repository. The supported
  route is to pull (mirror) into a new `archive-zstd` repository.
- Changing the default mode of `ostree init` or `ostree create-usb`.
- `ostree export` tar filters.

## 4. Why a new mode

Object checksums are computed over the uncompressed canonical form, so the
compressor is a storage and transport detail: commits, dirtrees, checksums
and signatures are identical between `archive` and `archive-zstd`.

Three ways to introduce the new compressor were considered.

| Approach | Old-client behaviour | Verdict |
|---|---|---|
| New mode `archive-zstd`, new object suffix | Refuses at config parse: "Invalid mode 'archive-zstd'" | Chosen |
| Keep `mode=archive-z2`, add `[archive] compression=zstd` | Ignores the key, then hits 404s or inflate errors mid-pull | Rejected |
| Keep `.filez`, detect the zstd magic in the payload | Inflate errors mid-pull; format becomes implicit | Rejected |

The third option is technically unambiguous (the zstd frame magic
`28 B5 2F FD` is not a valid raw deflate stream: it decodes as a stored
block whose `LEN`/`NLEN` check fails), and it would permit mixed
repositories. It is rejected because failure on old clients is late and
confusing, and because a mode string is the mechanism ostree already uses to
describe object layout.

## 5. Format

### 5.1 Repository mode

- Config: `[core] mode=archive-zstd`.
- New public enum value `OSTREE_REPO_MODE_ARCHIVE_ZSTD`, appended after
  `OSTREE_REPO_MODE_BARE_SPLIT_XATTRS` so existing values keep their
  numbers.
- `ostree_repo_mode_to_string()` and `ostree_repo_mode_from_string()` map it
  to and from `"archive-zstd"`.

### 5.2 Content objects

Path: `objects/XX/REST.filezst` (the `archive` equivalent is `.filez`).
Metadata objects (`.commit`, `.dirtree`, `.dirmeta`, ...) are unchanged and
uncompressed, as in `archive`.

Layout, identical to `.filez` except for the payload:

```
u32 BE   header size
u32      zero padding
bytes    GVariant (tuuuusa(ayay)): size, uid, gid, mode, rdev, symlink target, xattrs
bytes    payload: one standard zstd frame (regular files only)
```

The header GVariant type is the existing
`_OSTREE_ZLIB_FILE_HEADER_GVARIANT_FORMAT`, reused unchanged. The payload is
a normal zstd frame with the standard magic, so `tail -c +N obj | zstd -d`
recovers the file. Symlinks have a header and no payload. An empty regular
file has a header followed by a valid empty zstd frame.

A distinct suffix means a web server or CDN can never serve one format where
the other is expected, and a client can tell the format from the path alone.
`.filezst` fits comfortably in `_OSTREE_LOOSE_PATH_MAX` (256).

### 5.3 Compression parameters

- `[archive] zstd-level`: integer, clamped to 1–19, default 3. Levels 20–22
  are excluded because they raise the window size and therefore the memory a
  client needs to decompress.
- `[archive] zlib-level` is ignored in `archive-zstd` repositories, and
  `zstd-level` is ignored in `archive` repositories.
- The decompressor keeps libzstd's default maximum window (128 MiB,
  `ZSTD_WINDOWLOG_LIMIT_DEFAULT`), which bounds memory when reading data
  from an untrusted remote. A frame demanding more is rejected as corrupt.

## 6. Implementation

### 6.1 Build

- `configure.ac`: `--with-zstd` (default: auto). When enabled, check
  `libzstd >= 1.4.0` with pkg-config, define `HAVE_ZSTD`, add `zstd` to
  `OSTREE_FEATURES` (visible in `ostree --version`) and to
  `Requires.private`.
- `Makefile-libostree.am`: compile the new converter sources and link
  libzstd only when enabled.

### 6.2 Converters

New files, modelled on `ostree-lzma-compressor.[ch]` and
`ostree-lzma-decompressor.[ch]`:

- `src/libostree/ostree-zstd-compressor.[ch]` — `OstreeZstdCompressor`, a
  `GConverter` wrapping `ZSTD_CCtx` / `ZSTD_compressStream2`, with a `level`
  property. `G_CONVERTER_INPUT_AT_END` maps to `ZSTD_e_end`,
  `G_CONVERTER_FLUSH` to `ZSTD_e_flush`.
- `src/libostree/ostree-zstd-decompressor.[ch]` — `OstreeZstdDecompressor`,
  wrapping `ZSTD_DCtx` / `ZSTD_decompressStream`. libzstd errors become
  `G_IO_ERROR_INVALID_DATA`. Input that ends in the middle of a frame is
  reported as `G_IO_ERROR_PARTIAL_INPUT`, so a truncated object fails
  instead of yielding a short file.

Both are private (not exported, not in the `.sym` files).

### 6.3 Core helpers

In `ostree-core-private.h`:

- `_ostree_repo_mode_is_archive (mode)` — true for `ARCHIVE` and
  `ARCHIVE_ZSTD`. Replaces the direct `== OSTREE_REPO_MODE_ARCHIVE`
  comparisons wherever the code means "content is stored compressed with an
  inline header", which is nearly all of them (commit, checkout, the
  uncompressed-objects cache, devino scanning, fsck/prune enumeration,
  static-delta refusal on pull).
- `_ostree_archive_compressor_new (mode, level)` and
  `_ostree_archive_decompressor_new (mode)` — return the zlib or zstd
  `GConverter` for a mode. These are the only places that choose between
  compressors, so callers stay free of `#ifdef`.
- `_ostree_loose_path()` picks the suffix from the mode: `""`, `"z"` or
  `"zst"`.
- `_ostree_content_stream_parse_archive (mode, ...)` — the body of today's
  `ostree_content_stream_parse()`, taking a repo mode instead of a
  `gboolean compressed`. The public function keeps its signature and
  meaning (`compressed == TRUE` means zlib) and calls the new helper.

### 6.4 Call sites

- Write (`ostree-repo-commit.c`, `write_content_object`): use the
  mode-selected compressor and level.
- Read (`ostree-repo.c`, `repo_load_file_archive`): use the mode-selected
  parse.
- Object enumeration (`ostree-repo.c`, loose object listing): accept
  `.filezst` in `archive-zstd` repositories.
- Config (`ostree-repo.c`, `reload_config_inner`): parse `zstd-level`.
- Checkout, commit devino cache, payload-link and size accounting: switch to
  `_ostree_repo_mode_is_archive()`.
- `ostree_raw_file_to_archive_z2_stream*()` are public and stay zlib-only.
  Their internal callers that feed a repository use a new private
  mode-aware equivalent.

### 6.5 Pull

- The remote-mode check accepts `ARCHIVE` and `ARCHIVE_ZSTD`. The error for
  other modes is unchanged.
- Object URLs are built with the remote's mode, so content objects are
  requested as `.filezst` from an `archive-zstd` remote.
- Fetched content objects are parsed with the remote's mode.
- The mirror fast path (`trusted_http_direct`: copy the fetched file
  straight into the object store) is taken only when the local mode equals
  the remote mode. Mirroring between the two archive modes falls back to
  the normal decompress, checksum and recompress path, which is how a
  repository is converted.
- Pulling into an `archive-zstd` repository disables static deltas, as it
  does for `archive`.
- Local-repo pulls and `pull-local` go through the import path, which
  already hardlinks only when modes match.

### 6.6 Builds without zstd

`OSTREE_REPO_MODE_ARCHIVE_ZSTD` and the `"archive-zstd"` string always
exist, so the API does not vary with build options. Without `HAVE_ZSTD`:

- Opening or initialising an `archive-zstd` repository fails with
  "This build of ostree does not support archive-zstd repositories".
- Pulling from an `archive-zstd` remote fails with the same message at the
  remote-mode check, before any object is fetched.

### 6.7 API and bindings

- One new public enum value, documented with `Since: 2026.5`. No new
  exported functions, so the `.sym` files do not change.
- The Rust bindings regenerate from GIR; `rust-bindings/sys/tests/constant.c`
  gains the new constant.

## 7. Compatibility

| Client | Remote | Result |
|---|---|---|
| any | `archive` | unchanged |
| new, with zstd | `archive-zstd` | works |
| new, without zstd | `archive-zstd` | clear "not supported in this build" error |
| old | `archive-zstd` | "Invalid mode 'archive-zstd' in repository configuration" |

An operator serving old clients must keep an `archive` repository. Because
object IDs and signatures are identical, the two can be kept in sync by
mirroring one into the other.

## 8. Error handling

- Corrupt or truncated zstd payloads surface as `GError`s from the
  converter and fail the load, checkout, fsck or pull, as corrupt deflate
  does today. `fsck` reports the object as corrupted.
- An out-of-range or non-numeric `zstd-level` is clamped, matching
  `zlib-level`.

## 9. Testing

- `tests/test-zstd.c` (GTest, like `tests/test-lzma.c`): round-trip of
  empty, small and multi-megabyte random and compressible buffers through
  the converters; truncated input fails; garbage input fails.
- `tests/test-archive-zstd.sh`: `setup_test_repository "archive-zstd"` then
  the shared `archive-test.sh` and `basic-test.sh` bodies, plus checks that
  objects are named `.filezst` and begin their payload with the zstd magic.
- `tests/test-pull-archive-zstd.sh`: `setup_fake_remote_repo1
  "archive-zstd"` then the shared `pull-test.sh` body into a `bare-user`
  repository (the same shape as `test-pull-bareuser.sh`), plus mirror
  `archive` → `archive-zstd` and back, `fsck` each result, and compare
  commit checksums.
- `tests/libtest.sh`: teach the object-suffix helper about `archive-zstd`.
- All new shell tests start with `skip_without_ostree_feature zstd`.
- Regression: the existing `archive` tests (`test-archivez.sh`,
  `test-pull-bareuser.sh`, `test-pull-bare.sh`, `test-basic.sh`,
  `test-local-pull.sh`, `test-pull-mirror-summary.sh`) must still pass,
  with and without `--with-zstd`.

## 10. Documentation

- `man/ostree.repo-config.xml`: add `archive-zstd` to `mode`; document
  `zstd-level`, and `zlib-level`, which is currently undocumented.
- `man/ostree-init.xml`: list the new mode.
- `docs/formats.md`: describe the `archive-zstd` object layout.
- `ostree-core.h` and the `ostree-repo.c` class docs: describe the new enum
  value.
