# archive-zstd Repository Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an `archive-zstd` ostree repository mode whose content objects are `.filezst` files compressed with zstd, usable for commit, checkout, fsck, prune and pull.

**Architecture:** A new `OstreeRepoMode` value selects a zstd `GConverter` pair in place of GLib's zlib converters. Two private helpers (`_ostree_repo_mode_is_archive`, `_ostree_archive_{compressor,decompressor}_new`) are the only places that distinguish the two archive flavours; every other call site asks "is this an archive mode?". Object paths get their suffix from the mode, so pull requests `.filezst` from an `archive-zstd` remote.

**Tech Stack:** C (GLib/GIO `GConverter`), libzstd >= 1.4.0 streaming API (`ZSTD_compressStream2`, `ZSTD_decompressStream`), autotools, GTest, TAP shell tests.

**Spec:** `docs/superpowers/specs/2026-10-09-archive-zstd-design.md`

## Global Constraints

- Mode string is exactly `archive-zstd`; enum value is `OSTREE_REPO_MODE_ARCHIVE_ZSTD`, appended after `OSTREE_REPO_MODE_BARE_SPLIT_XATTRS` (numeric value 5).
- Content object suffix is exactly `.filezst`.
- Config key is `[archive] zstd-level`, clamped to 1–19, default 3.
- pkg-config dependency is `libzstd >= 1.4.0`; configure switch is `--with-zstd` (default auto); feature string is `zstd`; preprocessor symbol is `HAVE_ZSTD`; automake conditional is `USE_ZSTD`.
- Error text without libzstd is exactly `This build of ostree does not support archive-zstd repositories`.
- No new exported symbols: do not touch `src/libostree/*.sym`.
- Existing `archive` (`archive-z2`) behaviour, the `.filez` format and the public zlib functions (`ostree_raw_file_to_archive_z2_stream*`, `ostree_content_stream_parse`) must not change.
- Static deltas, multi-threading and `ostree export` are out of scope.
- New files carry the header `SPDX-License-Identifier: LGPL-2.0+`, matching neighbouring files.
- Code style: GNU style as formatted by the repo's `.clang-format`; run `clang-format -i` on new C files only.
- Never push to `origin` (upstream `ostreedev/ostree`).

## Build environment

Every build and test command below runs inside this nix shell (libzstd is not visible to the system pkg-config):

```bash
nix develop --impure -f /home/blake/.claude/jobs/41ab5e27/tmp/shell.nix
```

`shell.nix` contents (recreate if the job tmp directory is gone):

```nix
let
  pkgs = (builtins.getFlake "nixpkgs").legacyPackages.${builtins.currentSystem};
in
pkgs.mkShell {
  inputsFrom = [ pkgs.ostree ];
  packages = with pkgs; [ autoconf automake libtool pkg-config bison zstd python3 curl ];
}
```

First-time setup, from the worktree root:

```bash
git submodule update --init
env NOCONFIGURE=1 ./autogen.sh
./configure --with-curl --with-zstd
make -j"$(nproc)"
```

Run one test: `make check TESTS="tests/test-zstd"`. Logs land in `tests/<name>.log` (C tests) or `<name>.log` beside the script name printed by the harness.

Known environment noise (from earlier work on this machine): `make check -j` produces spurious "Could not connect" failures from the localhost test httpd, so run pull tests serially. Six tests fail regardless of changes: `test-core.js`, `test-pull-corruption.sh`, `test-remotes-config-dir.js`, `test-sizes.js`, `test-symbols.sh`, `test-sysroot.js`.

## Review Focus

1. A truncated `.filezst` object (interrupted upload, partial rsync) must make `fsck` and `checkout` fail, never produce a silently short file. Pinned in Task 1 (unit) and Task 3 (shell).
2. A corrupted payload (bit-flip, or a zlib `.filez` renamed to `.filezst`) must fail with an error, not crash or loop. Pinned in Task 1 (unit) and Task 3 (shell).
3. Empty regular files and symlinks, which have no or minimal payload, must round-trip. Pinned in Task 3.
4. Mirroring between `archive` and `archive-zstd` must recompress; it must never store a zstd payload under a `.filez` name or the reverse. Pinned in Task 4.
5. Out-of-range or non-numeric `zstd-level` values (`0`, `99`, `abc`) must be clamped and still produce readable objects. Pinned in Task 3.

## File Structure

| File | Responsibility |
|---|---|
| `src/libostree/ostree-zstd-compressor.[ch]` (new) | `OstreeZstdCompressor` GConverter |
| `src/libostree/ostree-zstd-decompressor.[ch]` (new) | `OstreeZstdDecompressor` GConverter |
| `tests/test-zstd.c` (new) | converter unit tests |
| `tests/test-archive-zstd.sh` (new) | repo-level test of the mode |
| `tests/test-pull-archive-zstd.sh` (new) | shared pull suite against an `archive-zstd` remote |
| `tests/test-pull-mirror-archive-zstd.sh` (new) | cross-mode mirror conversion |
| `configure.ac`, `Makefile-libostree.am`, `Makefile-tests.am` | build wiring |
| `src/libostree/ostree-core.h` | public enum value |
| `src/libostree/ostree-core-private.h`, `ostree-core.c` | mode helpers, paths, stream parse |
| `src/libostree/ostree-repo.c`, `ostree-repo-private.h` | mode strings, config, load, enumerate |
| `src/libostree/ostree-repo-commit.c`, `ostree-repo-checkout.c` | write path, cache checks |
| `src/libostree/ostree-repo-pull.c` | remote mode acceptance, URLs, parse |
| `tests/libtest.sh` | object-path helper suffix |
| `man/*.xml`, `docs/formats.md`, `src/ostree/ot-builtin-init.c` | documentation |
| `rust-bindings/sys/{src/lib.rs,tests/abi.rs,tests/constant.c}`, `rust-bindings/src/auto/enums.rs` | binding constants |

---

### Task 1: Build wiring and zstd converters

**Files:**
- Modify: `configure.ac` (after the zlib block at lines 143-146; summary block near line 755)
- Modify: `Makefile-libostree.am` (after the `USE_LIBARCHIVE` block near line 206)
- Modify: `Makefile-tests.am` (after `tests_test_lzma_LDADD`, line 424)
- Create: `src/libostree/ostree-zstd-compressor.h`, `src/libostree/ostree-zstd-compressor.c`
- Create: `src/libostree/ostree-zstd-decompressor.h`, `src/libostree/ostree-zstd-decompressor.c`
- Test: `tests/test-zstd.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `OstreeZstdCompressor *_ostree_zstd_compressor_new (int level);`
  - `OstreeZstdDecompressor *_ostree_zstd_decompressor_new (void);`
  - both implement `GConverter`; `HAVE_ZSTD`, `USE_ZSTD`, `OT_DEP_ZSTD_CFLAGS`, `OT_DEP_ZSTD_LIBS`.

- [ ] **Step 1: Add the configure check**

In `configure.ac`, directly after the line `REQUIRES_PRIVATE="${REQUIRES_PRIVATE} ${ZLIB_DEPENDENCY}"`:

```m4
dnl Optional: zstd compression for archive-zstd repositories
ZSTD_DEPENDENCY="libzstd >= 1.4.0"
AC_ARG_WITH([zstd],
  [AS_HELP_STRING([--with-zstd], [Enable archive-zstd repository support @<:@default=auto@:>@])],
  [], [with_zstd=auto])
AS_IF([test x$with_zstd != xno], [
  PKG_CHECK_MODULES([OT_DEP_ZSTD], [$ZSTD_DEPENDENCY], [have_zstd=yes], [have_zstd=no])
  AS_IF([test x$have_zstd = xno && test x$with_zstd = xyes],
    [AC_MSG_ERROR([--with-zstd was given, but $ZSTD_DEPENDENCY was not found])])
  with_zstd=$have_zstd
])
AS_IF([test x$with_zstd = xyes], [
  AC_DEFINE([HAVE_ZSTD], 1, [Define if we have libzstd])
  REQUIRES_PRIVATE="${REQUIRES_PRIVATE} ${ZSTD_DEPENDENCY}"
  OSTREE_FEATURES="$OSTREE_FEATURES zstd"
])
AM_CONDITIONAL([USE_ZSTD], [test x$with_zstd = xyes])
```

In the final `echo` summary, after the `libarchive (parse tar files directly):` line, add:

```
    zstd (archive-zstd repositories):             $with_zstd
```

- [ ] **Step 2: Wire the sources into the library and the unit test**

In `Makefile-libostree.am`, after the `if USE_LIBARCHIVE ... endif` block that appends `OT_DEP_LIBARCHIVE_*`:

```make
if USE_ZSTD
libostree_1_la_SOURCES += \
	src/libostree/ostree-zstd-compressor.c \
	src/libostree/ostree-zstd-compressor.h \
	src/libostree/ostree-zstd-decompressor.c \
	src/libostree/ostree-zstd-decompressor.h \
	$(NULL)
libostree_1_la_CFLAGS += $(OT_DEP_ZSTD_CFLAGS)
libostree_1_la_LIBADD += $(OT_DEP_ZSTD_LIBS)
endif
```

In `Makefile-tests.am`, after `tests_test_lzma_LDADD = ...`:

```make
if USE_ZSTD
_installed_or_uninstalled_test_programs += tests/test-zstd
tests_test_zstd_SOURCES = src/libostree/ostree-zstd-compressor.c \
	src/libostree/ostree-zstd-decompressor.c tests/test-zstd.c
tests_test_zstd_CFLAGS = $(TESTS_CFLAGS) $(OT_DEP_ZSTD_CFLAGS)
tests_test_zstd_LDADD = $(TESTS_LDADD) $(OT_DEP_ZSTD_LIBS)
endif
```

- [ ] **Step 3: Write the failing unit test**

Create `tests/test-zstd.c`:

```c
/*
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "libglnx.h"
#include "ostree-zstd-compressor.h"
#include "ostree-zstd-decompressor.h"
#include <gio/gio.h>
#include <glib.h>
#include <string.h>

static const guint8 zstd_magic[] = { 0x28, 0xB5, 0x2F, 0xFD };

static GBytes *
compress_bytes (const guint8 *data, gsize data_size, int level)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GInputStream) in = g_memory_input_stream_new_from_data (data, data_size, NULL);
  g_autoptr (GConverter) compressor = (GConverter *)_ostree_zstd_compressor_new (level);
  g_autoptr (GInputStream) convin = g_converter_input_stream_new (in, compressor);
  g_autoptr (GOutputStream) out = g_memory_output_stream_new_resizable ();
  gssize n = g_output_stream_splice (
      out, convin, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE, NULL,
      &error);
  g_assert_no_error (error);
  g_assert_cmpint (n, >, 0);
  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
}

/* Returns NULL and sets @error on failure */
static GBytes *
decompress_bytes (GBytes *compressed, GError **error)
{
  g_autoptr (GInputStream) in = g_memory_input_stream_new_from_bytes (compressed);
  g_autoptr (GConverter) decompressor = (GConverter *)_ostree_zstd_decompressor_new ();
  g_autoptr (GInputStream) convin = g_converter_input_stream_new (in, decompressor);
  g_autoptr (GOutputStream) out = g_memory_output_stream_new_resizable ();
  if (g_output_stream_splice (
          out, convin, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
          NULL, error)
      < 0)
    return NULL;
  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
}

static void
assert_roundtrip (const guint8 *data, gsize data_size, int level)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) compressed = compress_bytes (data, data_size, level);

  /* Always a standard zstd frame, even for empty input */
  g_assert_cmpint (g_bytes_get_size (compressed), >=, sizeof (zstd_magic));
  g_assert_cmpint (memcmp (g_bytes_get_data (compressed, NULL), zstd_magic, sizeof (zstd_magic)),
                   ==, 0);

  g_autoptr (GBytes) result = decompress_bytes (compressed, &error);
  g_assert_no_error (error);
  g_assert_nonnull (result);
  g_assert_cmpmem (g_bytes_get_data (result, NULL), g_bytes_get_size (result), data, data_size);
}

static void
test_zstd_empty (void)
{
  assert_roundtrip ((const guint8 *)"", 0, 3);
}

static void
test_zstd_random (void)
{
  guint8 buffer[4096];
  g_autoptr (GRand) r = g_rand_new_with_seed (42);
  for (gsize i = 0; i < sizeof (buffer); i++)
    buffer[i] = g_rand_int (r);

  for (gsize i = 2; i < (sizeof (buffer) - 1); i *= 2)
    {
      assert_roundtrip (buffer, i - 1, 3);
      assert_roundtrip (buffer, i, 3);
      assert_roundtrip (buffer, i + 1, 3);
    }
}

/* Larger than any internal zstd or GConverterStream buffer, in both the
 * highly compressible and the incompressible case. */
static void
test_zstd_big_buffers (void)
{
  const gsize buffer_size = 8 * 1024 * 1024;
  g_autofree guint8 *buffer = g_malloc (buffer_size);

  memset (buffer, 'a', buffer_size);
  assert_roundtrip (buffer, buffer_size, 3);

  g_autoptr (GRand) r = g_rand_new_with_seed (7);
  for (gsize i = 0; i < buffer_size; i++)
    buffer[i] = g_rand_int (r);
  assert_roundtrip (buffer, buffer_size, 3);
}

static void
test_zstd_levels (void)
{
  const gsize buffer_size = 256 * 1024;
  g_autofree guint8 *buffer = g_malloc (buffer_size);
  for (gsize i = 0; i < buffer_size; i++)
    buffer[i] = "the quick brown fox "[i % 20];

  assert_roundtrip (buffer, buffer_size, 1);
  assert_roundtrip (buffer, buffer_size, 19);

  g_autoptr (GBytes) fast = compress_bytes (buffer, buffer_size, 1);
  g_assert_cmpint (g_bytes_get_size (fast), <, buffer_size / 10);
}

/* A stream cut off mid-frame must be an error, not a short result */
static void
test_zstd_truncated (void)
{
  const gsize buffer_size = 1024 * 1024;
  g_autofree guint8 *buffer = g_malloc (buffer_size);
  g_autoptr (GRand) r = g_rand_new_with_seed (11);
  for (gsize i = 0; i < buffer_size; i++)
    buffer[i] = g_rand_int (r);

  g_autoptr (GBytes) compressed = compress_bytes (buffer, buffer_size, 3);
  const gsize full = g_bytes_get_size (compressed);
  const gsize cuts[] = { 0, 2, sizeof (zstd_magic), full / 2, full - 1 };

  for (gsize i = 0; i < G_N_ELEMENTS (cuts); i++)
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (GBytes) cut = g_bytes_new_from_bytes (compressed, 0, cuts[i]);
      g_autoptr (GBytes) result = decompress_bytes (cut, &error);
      g_assert_null (result);
      g_assert_nonnull (error);
    }
}

static void
test_zstd_garbage (void)
{
  g_autoptr (GError) error = NULL;
  guint8 garbage[1024];
  memset (garbage, 0x5a, sizeof (garbage));
  g_autoptr (GBytes) bytes = g_bytes_new_static (garbage, sizeof (garbage));
  g_autoptr (GBytes) result = decompress_bytes (bytes, &error);
  g_assert_null (result);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

/* Valid frame with a flipped byte in the middle */
static void
test_zstd_corrupted (void)
{
  const gsize buffer_size = 1024 * 1024;
  g_autofree guint8 *buffer = g_malloc (buffer_size);
  g_autoptr (GRand) r = g_rand_new_with_seed (13);
  for (gsize i = 0; i < buffer_size; i++)
    buffer[i] = g_rand_int_range (r, 0, 16);

  g_autoptr (GBytes) compressed = compress_bytes (buffer, buffer_size, 3);
  gsize len;
  g_autofree guint8 *copy = g_memdup2 (g_bytes_get_data (compressed, &len), len);
  /* Byte 5 is the block header of the first block */
  copy[5] ^= 0xff;
  copy[len / 2] ^= 0xff;

  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bad = g_bytes_new (copy, len);
  g_autoptr (GBytes) result = decompress_bytes (bad, &error);
  if (result != NULL)
    g_assert_false (g_bytes_get_size (result) == buffer_size
                    && memcmp (g_bytes_get_data (result, NULL), buffer, buffer_size) == 0);
  else
    g_assert_nonnull (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/zstd/empty", test_zstd_empty);
  g_test_add_func ("/zstd/random-buffer", test_zstd_random);
  g_test_add_func ("/zstd/big-buffers", test_zstd_big_buffers);
  g_test_add_func ("/zstd/levels", test_zstd_levels);
  g_test_add_func ("/zstd/truncated", test_zstd_truncated);
  g_test_add_func ("/zstd/garbage", test_zstd_garbage);
  g_test_add_func ("/zstd/corrupted", test_zstd_corrupted);

  return g_test_run ();
}
```

- [ ] **Step 4: Run it to make sure it fails**

Run: `env NOCONFIGURE=1 ./autogen.sh && ./configure --with-curl --with-zstd && make check TESTS="tests/test-zstd"`
Expected: build FAILS with `ostree-zstd-compressor.h: No such file or directory`. Also confirm the configure summary printed `zstd (archive-zstd repositories):             yes` and `features:` includes `zstd`.

- [ ] **Step 5: Write the compressor**

Create `src/libostree/ostree-zstd-compressor.h`:

```c
/*
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_ZSTD_COMPRESSOR (_ostree_zstd_compressor_get_type ())
#define OSTREE_ZSTD_COMPRESSOR(o) \
  (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_ZSTD_COMPRESSOR, OstreeZstdCompressor))
#define OSTREE_IS_ZSTD_COMPRESSOR(o) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_ZSTD_COMPRESSOR))

typedef struct _OstreeZstdCompressorClass OstreeZstdCompressorClass;
typedef struct _OstreeZstdCompressor OstreeZstdCompressor;

struct _OstreeZstdCompressorClass
{
  GObjectClass parent_class;
};

GType _ostree_zstd_compressor_get_type (void);

OstreeZstdCompressor *_ostree_zstd_compressor_new (int level);

G_END_DECLS
```

Create `src/libostree/ostree-zstd-compressor.c`:

```c
/*
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-zstd-compressor.h"

#include <zstd.h>

/**
 * SECTION:ostree-zstd-compressor
 * @title: zstd compressor
 *
 * An implementation of #GConverter that compresses data into a single
 * standard zstd frame.
 */

static void _ostree_zstd_compressor_iface_init (GConverterIface *iface);

struct _OstreeZstdCompressor
{
  GObject parent_instance;

  int level;
  ZSTD_CCtx *cctx;
};

G_DEFINE_TYPE_WITH_CODE (OstreeZstdCompressor, _ostree_zstd_compressor, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_CONVERTER,
                                                _ostree_zstd_compressor_iface_init))

static void
_ostree_zstd_compressor_finalize (GObject *object)
{
  OstreeZstdCompressor *self = OSTREE_ZSTD_COMPRESSOR (object);

  ZSTD_freeCCtx (self->cctx);

  G_OBJECT_CLASS (_ostree_zstd_compressor_parent_class)->finalize (object);
}

static void
_ostree_zstd_compressor_init (OstreeZstdCompressor *self)
{
  self->level = ZSTD_CLEVEL_DEFAULT;
}

static void
_ostree_zstd_compressor_class_init (OstreeZstdCompressorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = _ostree_zstd_compressor_finalize;
}

OstreeZstdCompressor *
_ostree_zstd_compressor_new (int level)
{
  OstreeZstdCompressor *self = g_object_new (OSTREE_TYPE_ZSTD_COMPRESSOR, NULL);
  self->level = level;
  return self;
}

static void
_ostree_zstd_compressor_reset (GConverter *converter)
{
  OstreeZstdCompressor *self = OSTREE_ZSTD_COMPRESSOR (converter);

  if (self->cctx)
    ZSTD_CCtx_reset (self->cctx, ZSTD_reset_session_only);
}

static GConverterResult
_ostree_zstd_compressor_convert (GConverter *converter, const void *inbuf, gsize inbuf_size,
                                 void *outbuf, gsize outbuf_size, GConverterFlags flags,
                                 gsize *bytes_read, gsize *bytes_written, GError **error)
{
  OstreeZstdCompressor *self = OSTREE_ZSTD_COMPRESSOR (converter);

  if (!self->cctx)
    {
      self->cctx = ZSTD_createCCtx ();
      if (!self->cctx)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to allocate zstd compression context");
          return G_CONVERTER_ERROR;
        }
      size_t r = ZSTD_CCtx_setParameter (self->cctx, ZSTD_c_compressionLevel, self->level);
      if (ZSTD_isError (r))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "Invalid zstd compression level %d: %s", self->level,
                       ZSTD_getErrorName (r));
          return G_CONVERTER_ERROR;
        }
    }

  ZSTD_inBuffer in = { inbuf, inbuf_size, 0 };
  ZSTD_outBuffer out = { outbuf, outbuf_size, 0 };

  ZSTD_EndDirective op = ZSTD_e_continue;
  if (flags & G_CONVERTER_INPUT_AT_END)
    op = ZSTD_e_end;
  else if (flags & G_CONVERTER_FLUSH)
    op = ZSTD_e_flush;

  size_t remaining = ZSTD_compressStream2 (self->cctx, &out, &in, op);
  if (ZSTD_isError (remaining))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "zstd compression failed: %s",
                   ZSTD_getErrorName (remaining));
      return G_CONVERTER_ERROR;
    }

  *bytes_read = in.pos;
  *bytes_written = out.pos;

  /* All input consumed and everything flushed into @outbuf */
  if (remaining == 0 && in.pos == in.size)
    {
      if (op == ZSTD_e_end)
        return G_CONVERTER_FINISHED;
      if (op == ZSTD_e_flush)
        return G_CONVERTER_FLUSHED;
    }

  /* GConverter requires progress or an error */
  if (in.pos == 0 && out.pos == 0)
    {
      if (out.size == 0 || remaining > 0)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Output buffer too small");
      else
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT, "Need more input");
      return G_CONVERTER_ERROR;
    }

  return G_CONVERTER_CONVERTED;
}

static void
_ostree_zstd_compressor_iface_init (GConverterIface *iface)
{
  iface->convert = _ostree_zstd_compressor_convert;
  iface->reset = _ostree_zstd_compressor_reset;
}
```

- [ ] **Step 6: Write the decompressor**

Create `src/libostree/ostree-zstd-decompressor.h` — identical to the compressor header with these substitutions: `COMPRESSOR` → `DECOMPRESSOR`, `Compressor` → `Decompressor`, `compressor` → `decompressor`, and the constructor line replaced by:

```c
OstreeZstdDecompressor *_ostree_zstd_decompressor_new (void);
```

Full text of the declarations section, for clarity:

```c
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_ZSTD_DECOMPRESSOR (_ostree_zstd_decompressor_get_type ())
#define OSTREE_ZSTD_DECOMPRESSOR(o) \
  (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_ZSTD_DECOMPRESSOR, OstreeZstdDecompressor))
#define OSTREE_IS_ZSTD_DECOMPRESSOR(o) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_ZSTD_DECOMPRESSOR))

typedef struct _OstreeZstdDecompressorClass OstreeZstdDecompressorClass;
typedef struct _OstreeZstdDecompressor OstreeZstdDecompressor;

struct _OstreeZstdDecompressorClass
{
  GObjectClass parent_class;
};

GType _ostree_zstd_decompressor_get_type (void);

OstreeZstdDecompressor *_ostree_zstd_decompressor_new (void);

G_END_DECLS
```

(Prefix it with the same licence comment as the compressor header.)

Create `src/libostree/ostree-zstd-decompressor.c` (same licence comment, then):

```c
#include "config.h"

#include "ostree-zstd-decompressor.h"

#include <zstd.h>

/**
 * SECTION:ostree-zstd-decompressor
 * @title: zstd decompressor
 *
 * An implementation of #GConverter that decompresses a single zstd frame.
 *
 * The decoder keeps libzstd's default window limit
 * (ZSTD_WINDOWLOG_LIMIT_DEFAULT, 128 MiB), which bounds the memory a
 * crafted object fetched from a remote can make us allocate.
 */

static void _ostree_zstd_decompressor_iface_init (GConverterIface *iface);

struct _OstreeZstdDecompressor
{
  GObject parent_instance;

  ZSTD_DCtx *dctx;
};

G_DEFINE_TYPE_WITH_CODE (OstreeZstdDecompressor, _ostree_zstd_decompressor, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_CONVERTER,
                                                _ostree_zstd_decompressor_iface_init))

static void
_ostree_zstd_decompressor_finalize (GObject *object)
{
  OstreeZstdDecompressor *self = OSTREE_ZSTD_DECOMPRESSOR (object);

  ZSTD_freeDCtx (self->dctx);

  G_OBJECT_CLASS (_ostree_zstd_decompressor_parent_class)->finalize (object);
}

static void
_ostree_zstd_decompressor_init (OstreeZstdDecompressor *self)
{
}

static void
_ostree_zstd_decompressor_class_init (OstreeZstdDecompressorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = _ostree_zstd_decompressor_finalize;
}

OstreeZstdDecompressor *
_ostree_zstd_decompressor_new (void)
{
  return g_object_new (OSTREE_TYPE_ZSTD_DECOMPRESSOR, NULL);
}

static void
_ostree_zstd_decompressor_reset (GConverter *converter)
{
  OstreeZstdDecompressor *self = OSTREE_ZSTD_DECOMPRESSOR (converter);

  if (self->dctx)
    ZSTD_DCtx_reset (self->dctx, ZSTD_reset_session_only);
}

static GConverterResult
_ostree_zstd_decompressor_convert (GConverter *converter, const void *inbuf, gsize inbuf_size,
                                   void *outbuf, gsize outbuf_size, GConverterFlags flags,
                                   gsize *bytes_read, gsize *bytes_written, GError **error)
{
  OstreeZstdDecompressor *self = OSTREE_ZSTD_DECOMPRESSOR (converter);

  if (!self->dctx)
    {
      self->dctx = ZSTD_createDCtx ();
      if (!self->dctx)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to allocate zstd decompression context");
          return G_CONVERTER_ERROR;
        }
    }

  ZSTD_inBuffer in = { inbuf, inbuf_size, 0 };
  ZSTD_outBuffer out = { outbuf, outbuf_size, 0 };

  /* Returns 0 once a frame is completely decoded and flushed, otherwise a
   * hint for how much more input it wants. */
  size_t ret = ZSTD_decompressStream (self->dctx, &out, &in);
  if (ZSTD_isError (ret))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "zstd decompression failed: %s",
                   ZSTD_getErrorName (ret));
      return G_CONVERTER_ERROR;
    }

  *bytes_read = in.pos;
  *bytes_written = out.pos;

  if (ret == 0)
    return G_CONVERTER_FINISHED;

  /* GConverter requires progress or an error */
  if (in.pos == 0 && out.pos == 0)
    {
      if (out.size == 0 || in.size > 0)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Output buffer too small");
      else
        /* No input left and the frame is unfinished: with
         * G_CONVERTER_INPUT_AT_END the stream wrappers turn this into a hard
         * error, so a truncated object can never read back as a short file. */
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                             "Truncated zstd stream");
      return G_CONVERTER_ERROR;
    }

  return G_CONVERTER_CONVERTED;
}

static void
_ostree_zstd_decompressor_iface_init (GConverterIface *iface)
{
  iface->convert = _ostree_zstd_decompressor_convert;
  iface->reset = _ostree_zstd_decompressor_reset;
}
```

- [ ] **Step 7: Format and run the tests**

Run:
```bash
clang-format -i src/libostree/ostree-zstd-*.[ch] tests/test-zstd.c
make -j"$(nproc)" && make check TESTS="tests/test-zstd"
```
Expected: `PASS: tests/test-zstd` with 7 subtests. If `/zstd/truncated` fails for the `cuts[0] == 0` case (zero-length input), inspect how `GConverterInputStream` reports EOF with no input and make the decompressor return `G_IO_ERROR_PARTIAL_INPUT` there too; do not weaken the test.

- [ ] **Step 8: Commit**

```bash
git add configure.ac Makefile-libostree.am Makefile-tests.am src/libostree/ostree-zstd-*.[ch] tests/test-zstd.c
git commit -m "lib: add optional libzstd dependency and zstd GConverters"
```

---

### Task 2: The `archive-zstd` mode, its strings and path helpers

**Files:**
- Modify: `src/libostree/ostree-core.h:192-217` (enum and its doc comment)
- Modify: `src/libostree/ostree-core-private.h` (near lines 29, 112, 159)
- Modify: `src/libostree/ostree-core.c` (lines 39-40, `_ostree_loose_path` ~1630, `_ostree_get_relative_object_path` ~1786)
- Modify: `src/libostree/ostree-repo.c` (`ostree_repo_mode_to_string` ~2590, `ostree_repo_mode_from_string` ~2624, `repo_create_at_internal` ~2685, `reload_core_config` ~3101)
- Modify: `src/libostree/ostree-repo-pull.c:1924`
- Test: `tests/test-basic-c.c`

**Interfaces:**
- Consumes: `HAVE_ZSTD` from Task 1.
- Produces:
  - `OSTREE_REPO_MODE_ARCHIVE_ZSTD` (public enum value, == 5)
  - `#define OSTREE_ARCHIVE_ZSTD_DEFAULT_COMPRESSION_LEVEL (3)`
  - `static inline gboolean _ostree_repo_mode_is_archive (OstreeRepoMode mode);`
  - `gboolean _ostree_repo_mode_check_supported (OstreeRepoMode mode, GError **error);`
  - `char *_ostree_get_relative_object_path (const char *checksum, OstreeObjectType type, OstreeRepoMode mode);` (third parameter changes from `gboolean compressed`)
  - `_ostree_loose_path()` emits `.filezst` for `OSTREE_REPO_MODE_ARCHIVE_ZSTD`.

- [ ] **Step 1: Write the failing test**

In `tests/test-basic-c.c`, add before `main`:

```c
static void
test_repo_mode_archive_zstd_strings (void)
{
  g_autoptr (GError) error = NULL;
  OstreeRepoMode mode = OSTREE_REPO_MODE_BARE;
  const char *str = NULL;

  g_assert_cmpint (OSTREE_REPO_MODE_ARCHIVE_ZSTD, ==, 5);

  g_assert_true (ostree_repo_mode_from_string ("archive-zstd", &mode, &error));
  g_assert_no_error (error);
  g_assert_cmpint (mode, ==, OSTREE_REPO_MODE_ARCHIVE_ZSTD);

  g_assert_true (ostree_repo_mode_to_string (OSTREE_REPO_MODE_ARCHIVE_ZSTD, &str, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (str, ==, "archive-zstd");

  /* The existing archive mode is untouched */
  g_assert_true (ostree_repo_mode_from_string ("archive", &mode, &error));
  g_assert_cmpint (mode, ==, OSTREE_REPO_MODE_ARCHIVE);
  g_assert_true (ostree_repo_mode_from_string ("archive-z2", &mode, &error));
  g_assert_cmpint (mode, ==, OSTREE_REPO_MODE_ARCHIVE);
  g_assert_true (ostree_repo_mode_to_string (OSTREE_REPO_MODE_ARCHIVE, &str, &error));
  g_assert_cmpstr (str, ==, "archive-z2");
}
```

and in `main`, after the `/list-static-delta-entries` line:

```c
  g_test_add_func ("/repo-mode-archive-zstd-strings", test_repo_mode_archive_zstd_strings);
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `make check TESTS="tests/test-basic-c"`
Expected: compile FAILS with `'OSTREE_REPO_MODE_ARCHIVE_ZSTD' undeclared`.

- [ ] **Step 3: Add the enum value**

In `src/libostree/ostree-core.h`, add to the doc comment after the `@OSTREE_REPO_MODE_BARE_SPLIT_XATTRS` entry:

```c
 * @OSTREE_REPO_MODE_ARCHIVE_ZSTD: Like `OSTREE_REPO_MODE_ARCHIVE`, but content objects are
 * compressed with zstd instead of zlib.  Requires libostree to be built with zstd support.
 * Since: 2026.5
```

and to the enum after `OSTREE_REPO_MODE_BARE_SPLIT_XATTRS,`:

```c
  OSTREE_REPO_MODE_ARCHIVE_ZSTD,
```

In `src/libostree/ostree-core.c`, after the two existing `G_STATIC_ASSERT` lines at 39-40:

```c
G_STATIC_ASSERT (OSTREE_REPO_MODE_ARCHIVE_ZSTD == 5);
```

- [ ] **Step 4: Add the private helpers**

In `src/libostree/ostree-core-private.h`, after `#define OSTREE_ARCHIVE_DEFAULT_COMPRESSION_LEVEL (6)`:

```c
/* The zstd library default; see docs/formats.md */
#define OSTREE_ARCHIVE_ZSTD_DEFAULT_COMPRESSION_LEVEL (3)
```

Change the `_ostree_get_relative_object_path` declaration to:

```c
char *_ostree_get_relative_object_path (const char *checksum, OstreeObjectType type,
                                        OstreeRepoMode mode);
```

After the `_ostree_repo_mode_is_bare` inline function:

```c
/* Archive modes store each content object as a header followed by a
 * compressed payload; they differ only in the compressor and file suffix. */
static inline gboolean
_ostree_repo_mode_is_archive (OstreeRepoMode mode)
{
  return mode == OSTREE_REPO_MODE_ARCHIVE || mode == OSTREE_REPO_MODE_ARCHIVE_ZSTD;
}

gboolean _ostree_repo_mode_check_supported (OstreeRepoMode mode, GError **error);
```

In `src/libostree/ostree-core.c`, replace the body of `_ostree_loose_path` with:

```c
void
_ostree_loose_path (char *buf, const char *checksum, OstreeObjectType objtype, OstreeRepoMode mode)
{
  const char *suffix = "";
  if (!OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      if (mode == OSTREE_REPO_MODE_ARCHIVE)
        suffix = "z";
      else if (mode == OSTREE_REPO_MODE_ARCHIVE_ZSTD)
        suffix = "zst";
    }

  *buf = checksum[0];
  buf++;
  *buf = checksum[1];
  buf++;
  snprintf (buf, _OSTREE_LOOSE_PATH_MAX - 2, "/%s.%s%s", checksum + 2,
            ostree_object_type_to_string (objtype), suffix);
}
```

Replace `_ostree_get_relative_object_path` (keep its doc comment, changing the `@compressed` line to `@mode: Repository mode, which determines the content object suffix`):

```c
char *
_ostree_get_relative_object_path (const char *checksum, OstreeObjectType type, OstreeRepoMode mode)
{
  char loose_path[_OSTREE_LOOSE_PATH_MAX];

  g_assert (strlen (checksum) == OSTREE_SHA256_STRING_LEN);

  _ostree_loose_path (loose_path, checksum, type, mode);
  return g_strconcat ("objects/", loose_path, NULL);
}
```

Add directly after it:

```c
/* Fail if @mode needs a compressor this build lacks. */
gboolean
_ostree_repo_mode_check_supported (OstreeRepoMode mode, GError **error)
{
#ifndef HAVE_ZSTD
  if (mode == OSTREE_REPO_MODE_ARCHIVE_ZSTD)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "This build of ostree does not support archive-zstd repositories");
      return FALSE;
    }
#endif
  return TRUE;
}
```

In `src/libostree/ostree-repo-pull.c:1924`, keep behaviour identical for now by passing the existing archive mode (Task 4 switches it to the remote's mode):

```c
      obj_subpath
          = _ostree_get_relative_object_path (expected_checksum, objtype, OSTREE_REPO_MODE_ARCHIVE);
```

- [ ] **Step 5: Add the mode strings and the support gate**

In `src/libostree/ostree-repo.c`, `ostree_repo_mode_to_string`, before `default:`:

```c
    case OSTREE_REPO_MODE_ARCHIVE_ZSTD:
      ret_mode = "archive-zstd";
      break;
```

In `ostree_repo_mode_from_string`, before the final `else`:

```c
  else if (strcmp (mode, "archive-zstd") == 0)
    ret_mode = OSTREE_REPO_MODE_ARCHIVE_ZSTD;
```

In `repo_create_at_internal`, immediately before `if (!ostree_repo_mode_to_string (mode, &mode_str, error))`:

```c
      if (!_ostree_repo_mode_check_supported (mode, error))
        return FALSE;
```

In the config loader, immediately after `if (!ostree_repo_mode_from_string (mode, &self->mode, error)) return FALSE;` (line ~3101):

```c
  if (!_ostree_repo_mode_check_supported (self->mode, error))
    return FALSE;
```

Also update the class documentation paragraph at `ostree-repo.c:95-97`: after the sentence ending "stores content files zlib-compressed.", add: `A %OSTREE_REPO_MODE_ARCHIVE_ZSTD repository is the same, but compresses with zstd.`

- [ ] **Step 6: Run the tests**

Run: `make -j"$(nproc)" && make check TESTS="tests/test-basic-c tests/test-archivez.sh"`
Expected: both PASS. `test-archivez.sh` proves the `.filez` path is byte-for-byte unchanged after the `_ostree_loose_path` rewrite.

- [ ] **Step 7: Commit**

```bash
git add src/libostree/ostree-core.h src/libostree/ostree-core-private.h src/libostree/ostree-core.c src/libostree/ostree-repo.c src/libostree/ostree-repo-pull.c tests/test-basic-c.c
git commit -m "lib: add OSTREE_REPO_MODE_ARCHIVE_ZSTD and mode-aware object paths"
```

---

### Task 3: Reading and writing `archive-zstd` repositories

**Files:**
- Modify: `src/libostree/ostree-core-private.h`, `src/libostree/ostree-core.c` (converter factories; `ostree_content_stream_parse` ~line 597)
- Modify: `src/libostree/ostree-repo-private.h:209`
- Modify: `src/libostree/ostree-repo.c` (config ~3166, enumeration ~3870, `repo_load_file_archive` ~4101, `ostree_repo_load_file` ~4314)
- Modify: `src/libostree/ostree-repo-commit.c` (lines 360, 1035, 1057-1100, 1518, 1566, 2832)
- Modify: `src/libostree/ostree-repo-checkout.c` (lines 786, 856)
- Modify: `tests/libtest.sh:862`
- Modify: `Makefile-tests.am` (script list near line 71)
- Test: `tests/test-archive-zstd.sh`

**Interfaces:**
- Consumes: `_ostree_zstd_compressor_new (int)`, `_ostree_zstd_decompressor_new (void)`, `_ostree_repo_mode_is_archive`, `OSTREE_ARCHIVE_ZSTD_DEFAULT_COMPRESSION_LEVEL`.
- Produces:
  - `GConverter *_ostree_archive_compressor_new (OstreeRepoMode mode, int level);`
  - `GConverter *_ostree_archive_decompressor_new (OstreeRepoMode mode);`
  - `gboolean _ostree_content_stream_parse_for_mode (OstreeRepoMode mode, GInputStream *input, guint64 input_length, gboolean trusted, GInputStream **out_input, GFileInfo **out_file_info, GVariant **out_xattrs, GCancellable *cancellable, GError **error);` — `mode` is the mode of the repository the stream came from; any bare mode means "uncompressed content stream".
  - `OstreeRepo.zstd_compression_level` (`guint`).

- [ ] **Step 1: Write the failing test**

Create `tests/test-archive-zstd.sh` (mode 0755):

```bash
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

echo '1..20'

setup_test_repository "archive-zstd"

# 11 tests shared with test-archivez.sh
. ${test_srcdir}/archive-test.sh

cd ${test_tmpdir}
assert_file_has_content repo/config '^mode=archive-zstd$'
find repo/objects -name '*.filez' > filez.txt
assert_not_file_has_content filez.txt .
find repo/objects -name '*.filezst' > filezst.txt
assert_file_has_content filezst.txt 'filezst$'
echo "ok objects are named .filezst"

# The payload after the header is one standard zstd frame
objpath=$(ostree_file_path_to_relative_object_path repo test2 /baz/cow)
assert_str_match "${objpath}" '\.filezst$'
python3 - repo/${objpath} <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read()
(hdrlen,) = struct.unpack('>I', data[:4])
payload = data[8 + hdrlen:]
assert payload[:4] == b'\x28\xb5\x2f\xfd', payload[:4]
open('cow.zst', 'wb').write(payload)
EOF
if command -v zstd >/dev/null; then
    zstd -q -d cow.zst -o cow.out
    assert_file_has_content cow.out '^moo$'
fi
echo "ok payload is a standard zstd frame"

$OSTREE fsck -q
echo "ok fsck"

# Empty files, symlinks, incompressible data and a multi-megabyte file
mkdir edge
: > edge/empty
ln -s empty edge/link
head -c 3000000 /dev/urandom > edge/random
python3 -c "import sys; sys.stdout.write('ostree zstd ' * 500000)" > edge/big
$OSTREE commit -b edge --tree=dir=edge >/dev/null
$OSTREE fsck -q
$OSTREE checkout -U edge edge-co
assert_has_file edge-co/empty
test ! -s edge-co/empty
assert_symlink_has_content edge-co/link '^empty$'
cmp edge/random edge-co/random
cmp edge/big edge-co/big
echo "ok empty file, symlink, random and large content round trip"

# zstd-level: out-of-range and junk values are clamped, not fatal
for level in 1 19 0 99 abc; do
    rm -rf repo-level co-level
    ostree_repo_init repo-level --mode=archive-zstd
    ${CMD_PREFIX} ostree --repo=repo-level config set archive.zstd-level ${level}
    ${CMD_PREFIX} ostree --repo=repo-level commit -b edge --tree=dir=edge >/dev/null
    ${CMD_PREFIX} ostree --repo=repo-level fsck -q
    ${CMD_PREFIX} ostree --repo=repo-level checkout -U edge co-level
    cmp edge/big co-level/big
    du -sb repo-level/objects | cut -f1 > size-${level}.txt
done
# Level 19 must compress the compressible file at least as well as level 1
test "$(cat size-19.txt)" -le "$(cat size-1.txt)"
echo "ok zstd-level is honoured and clamped"

# A truncated object must be an error, never a short file
rm -rf repo-trunc co-trunc
ostree_repo_init repo-trunc --mode=archive-zstd
${CMD_PREFIX} ostree --repo=repo-trunc commit -b edge --tree=dir=edge >/dev/null
bigobj=$(ostree_file_path_to_relative_object_path repo-trunc edge /random)
size=$(stat -c %s repo-trunc/${bigobj})
truncate -s $((size / 2)) repo-trunc/${bigobj}
if ${CMD_PREFIX} ostree --repo=repo-trunc fsck -q 2>err.txt; then
    assert_not_reached "fsck passed on a truncated .filezst"
fi
if ${CMD_PREFIX} ostree --repo=repo-trunc checkout -U edge co-trunc 2>err.txt; then
    assert_not_reached "checkout succeeded from a truncated .filezst"
fi
echo "ok truncated object is detected"

# A corrupted payload must be an error
rm -rf repo-corrupt
ostree_repo_init repo-corrupt --mode=archive-zstd
${CMD_PREFIX} ostree --repo=repo-corrupt commit -b edge --tree=dir=edge >/dev/null
bigobj=$(ostree_file_path_to_relative_object_path repo-corrupt edge /random)
size=$(stat -c %s repo-corrupt/${bigobj})
printf '\xff\xff\xff\xff\xff\xff\xff\xff' | dd of=repo-corrupt/${bigobj} bs=1 seek=$((size / 2)) conv=notrunc status=none
if ${CMD_PREFIX} ostree --repo=repo-corrupt fsck -q 2>err.txt; then
    assert_not_reached "fsck passed on a corrupted .filezst"
fi
echo "ok corrupted object is detected"

# A zlib payload under a .filezst name is rejected
rm -rf repo-z repo-mixed
ostree_repo_init repo-z --mode=archive
${CMD_PREFIX} ostree --repo=repo-z commit -b edge --tree=dir=edge >/dev/null
ostree_repo_init repo-mixed --mode=archive-zstd
${CMD_PREFIX} ostree --repo=repo-mixed commit -b edge --tree=dir=edge >/dev/null
zobj=$(ostree_file_path_to_relative_object_path repo-z edge /big)
zstobj=$(ostree_file_path_to_relative_object_path repo-mixed edge /big)
cp repo-z/${zobj} repo-mixed/${zstobj}
if ${CMD_PREFIX} ostree --repo=repo-mixed fsck -q 2>err.txt; then
    assert_not_reached "fsck passed on a zlib payload named .filezst"
fi
echo "ok zlib payload in a .filezst is rejected"

# Prune removes .filezst objects
rm -rf repo-prune
ostree_repo_init repo-prune --mode=archive-zstd
${CMD_PREFIX} ostree --repo=repo-prune commit -b edge --tree=dir=edge >/dev/null
${CMD_PREFIX} ostree --repo=repo-prune refs --delete edge
${CMD_PREFIX} ostree --repo=repo-prune prune --refs-only >/dev/null
find repo-prune/objects -name '*.filezst' > pruned.txt
assert_not_file_has_content pruned.txt .
echo "ok prune"
```

Register it in `Makefile-tests.am` on the line after `tests/test-archivez.sh \`:

```make
	tests/test-archive-zstd.sh \
```

In `tests/libtest.sh`, replace the suffix line in `ostree_checksum_to_relative_object_path`:

```bash
    if grep -Eq -e '^mode=archive-zstd' ${repo}/config; then suffix=zst
    elif grep -Eq -e '^mode=archive' ${repo}/config; then suffix=z
    else suffix=''; fi
```

The helpers used above all exist already: `ostree_file_path_to_relative_object_path` in `tests/libtest.sh`, and `assert_str_match`, `assert_symlink_has_content`, `assert_not_file_has_content`, `assert_not_reached` and `assert_streq` in `tests/libtest-core.sh`.

- [ ] **Step 2: Run it to make sure it fails**

Run: `chmod +x tests/test-archive-zstd.sh && make check TESTS="tests/test-archive-zstd.sh"`
Expected: FAIL. `init` succeeds (Task 2) but the first commit writes zlib data into a `.filezst`, or object enumeration skips `.filezst`, so `archive-test.sh` or the magic check fails.

- [ ] **Step 3: Add the converter factories and mode-aware parse**

In `src/libostree/ostree-core-private.h`, after the `_ostree_zlib_file_header_new` declaration:

```c
GConverter *_ostree_archive_compressor_new (OstreeRepoMode mode, int level);
GConverter *_ostree_archive_decompressor_new (OstreeRepoMode mode);

gboolean _ostree_content_stream_parse_for_mode (OstreeRepoMode mode, GInputStream *input,
                                                guint64 input_length, gboolean trusted,
                                                GInputStream **out_input,
                                                GFileInfo **out_file_info, GVariant **out_xattrs,
                                                GCancellable *cancellable, GError **error);
```

Update the comment above `_OSTREE_ZLIB_FILE_HEADER_GVARIANT_FORMAT`: change "storing zlib-compressed content objects." to "storing compressed (archive and archive-zstd) content objects." and the trailing "zlib-compressed data" to "zlib- or zstd-compressed data".

In `src/libostree/ostree-core.c`, add to the includes:

```c
#ifdef HAVE_ZSTD
#include "ostree-zstd-compressor.h"
#include "ostree-zstd-decompressor.h"
#endif
```

Add before `_ostree_raw_file_to_archive_stream`:

```c
/* The compressor for content objects in archive repository mode @mode.
 * Repositories whose mode this build can't handle are rejected when they are
 * opened; see _ostree_repo_mode_check_supported(). */
GConverter *
_ostree_archive_compressor_new (OstreeRepoMode mode, int level)
{
  g_assert (_ostree_repo_mode_is_archive (mode));
#ifdef HAVE_ZSTD
  if (mode == OSTREE_REPO_MODE_ARCHIVE_ZSTD)
    return G_CONVERTER (_ostree_zstd_compressor_new (level));
#endif
  g_assert (mode == OSTREE_REPO_MODE_ARCHIVE);
  return G_CONVERTER (g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, level));
}

GConverter *
_ostree_archive_decompressor_new (OstreeRepoMode mode)
{
  g_assert (_ostree_repo_mode_is_archive (mode));
#ifdef HAVE_ZSTD
  if (mode == OSTREE_REPO_MODE_ARCHIVE_ZSTD)
    return G_CONVERTER (_ostree_zstd_decompressor_new ());
#endif
  g_assert (mode == OSTREE_REPO_MODE_ARCHIVE);
  return G_CONVERTER (g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW));
}
```

Rename the existing `ostree_content_stream_parse` function body into the private function: change its signature line to

```c
gboolean
_ostree_content_stream_parse_for_mode (OstreeRepoMode mode, GInputStream *input,
                                       guint64 input_length, gboolean trusted,
                                       GInputStream **out_input, GFileInfo **out_file_info,
                                       GVariant **out_xattrs, GCancellable *cancellable,
                                       GError **error)
{
  const gboolean compressed = _ostree_repo_mode_is_archive (mode);
```

leave the rest of the body unchanged except the decompressor creation, which becomes:

```c
      if (compressed)
        {
          g_autoptr (GConverter) decomp = _ostree_archive_decompressor_new (mode);
          ret_input = g_converter_input_stream_new (input, decomp);
        }
```

Move the existing doc comment (`ostree_content_stream_parse: ...`) so it stays attached to the public function, which is re-added below the private one as:

```c
gboolean
ostree_content_stream_parse (gboolean compressed, GInputStream *input, guint64 input_length,
                             gboolean trusted, GInputStream **out_input, GFileInfo **out_file_info,
                             GVariant **out_xattrs, GCancellable *cancellable, GError **error)
{
  return _ostree_content_stream_parse_for_mode (
      compressed ? OSTREE_REPO_MODE_ARCHIVE : OSTREE_REPO_MODE_BARE, input, input_length, trusted,
      out_input, out_file_info, out_xattrs, cancellable, error);
}
```

- [ ] **Step 4: Config, load and enumeration in `ostree-repo.c`**

In `src/libostree/ostree-repo-private.h`, after `guint zlib_compression_level;`:

```c
  guint zstd_compression_level;
```

In `src/libostree/ostree-repo.c`, directly after the block that sets `self->zlib_compression_level` (ends ~line 3179):

```c
  {
    g_autofree char *compression_level_str = NULL;

    (void)ot_keyfile_get_value_with_default (self->config, "archive", "zstd-level", NULL,
                                             &compression_level_str, NULL);

    if (compression_level_str)
      /* Ensure level is in [1,19]; higher levels need much more memory to decompress */
      self->zstd_compression_level
          = MAX (1, MIN (19, g_ascii_strtoull (compression_level_str, NULL, 10)));
    else
      self->zstd_compression_level = OSTREE_ARCHIVE_ZSTD_DEFAULT_COMPRESSION_LEVEL;
  }
```

Object enumeration (~line 3870), replace the two-line condition with:

```c
      if ((self->mode == OSTREE_REPO_MODE_ARCHIVE && strcmp (dot, ".filez") == 0)
          || (self->mode == OSTREE_REPO_MODE_ARCHIVE_ZSTD && strcmp (dot, ".filezst") == 0)
          || ((_ostree_repo_mode_is_bare (self->mode)) && strcmp (dot, ".file") == 0))
```

`repo_load_file_archive` (~line 4101):

```c
      return _ostree_content_stream_parse_for_mode (self->mode, tmp_stream, stbuf.st_size, TRUE,
                                                    out_input, out_file_info, out_xattrs,
                                                    cancellable, error);
```

`ostree_repo_load_file` (~line 4314):

```c
  if (_ostree_repo_mode_is_archive (self->mode))
```

- [ ] **Step 5: Write path and mode checks in `ostree-repo-commit.c`**

- Line 360: `if (_ostree_repo_mode_is_archive (ostree_repo_get_mode (self)))`
- Line 1035: `else if (!_ostree_repo_mode_is_archive (repo_mode))`
- Lines 1057-1100, the archive branch of `write_content_object`: rename `zlib_compressor` to `compressor`, replace the assert and the compressor construction, and close the converter stream explicitly so that an error while writing the final compressed block is reported instead of being dropped when the stream is finalized:

```c
      g_autoptr (GConverter) compressor = NULL;
      g_autoptr (GOutputStream) compressed_out_stream = NULL;
      g_autoptr (GOutputStream) temp_out = NULL;

      g_assert (_ostree_repo_mode_is_archive (repo_mode));
```

```c
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          const int level = (repo_mode == OSTREE_REPO_MODE_ARCHIVE_ZSTD)
                                ? self->zstd_compression_level
                                : self->zlib_compression_level;
          compressor = _ostree_archive_compressor_new (repo_mode, level);
          compressed_out_stream = g_converter_output_stream_new (temp_out, compressor);
          /* Don't close the base; we'll do that later */
          g_filter_output_stream_set_close_base_stream (
              (GFilterOutputStream *)compressed_out_stream, FALSE);

          if (g_output_stream_splice (compressed_out_stream, file_input, 0, cancellable, error) < 0)
            return FALSE;

          /* Writes the end of the compressed stream */
          if (!g_output_stream_close (compressed_out_stream, cancellable, error))
            return FALSE;

          unpacked_size = g_file_info_get_size (file_info);
        }
```

- Line 1518, add a case label so the devino scan handles the new mode:

```c
            case OSTREE_REPO_MODE_ARCHIVE:
            case OSTREE_REPO_MODE_ARCHIVE_ZSTD:
```

- Line 1566: `if (_ostree_repo_mode_is_archive (self->mode) && self->uncompressed_objects_dir_fd != -1)`
- Line 2832: `if (_ostree_repo_mode_is_archive (self->mode))`

- [ ] **Step 6: Checkout cache checks in `ostree-repo-checkout.c`**

- Line 786: `= (_ostree_repo_mode_is_archive (current_repo->mode)`
- Line 856: `&& need_copy && _ostree_repo_mode_is_archive (repo->mode)`

- [ ] **Step 7: Sweep for missed call sites**

Run: `grep -rn "OSTREE_REPO_MODE_ARCHIVE\b" src/ | grep -v "ostree-core.h"`
Expected remaining hits, each intentional:
- `src/ostree/ot-builtin-create-usb.c:127` (default mode for USB repos, unchanged)
- `src/libostree/ostree-core.c` static asserts, `_ostree_loose_path`, the two factories and the public `ostree_content_stream_parse`
- `src/libostree/ostree-repo.c` mode strings, class docs and the `.filez` enumeration arm
- `src/libostree/ostree-repo-commit.c` the `case` label
- `src/libostree/ostree-repo-pull.c` lines ~1924, ~3981, ~3989, ~4517, ~5708 (Task 4)

Any other hit means a call site was missed: decide whether it means "zlib specifically" (leave) or "any archive mode" (convert to `_ostree_repo_mode_is_archive`).

- [ ] **Step 8: Run the tests**

Run: `make -j"$(nproc)" && make check TESTS="tests/test-archive-zstd.sh tests/test-archivez.sh tests/test-basic-c tests/test-basic-user.sh tests/test-prune.sh"`
Expected: all PASS; `test-archive-zstd.sh` reports 20 tests. If the `ok` count differs from 20, fix the `1..N` line to match the actual number of `echo "ok` lines (11 from `archive-test.sh` plus those in this file).

- [ ] **Step 9: Commit**

```bash
git add src/libostree tests/test-archive-zstd.sh tests/libtest.sh Makefile-tests.am
git commit -m "lib: read and write zstd-compressed content in archive-zstd repositories"
```

---

### Task 4: Pulling from and mirroring `archive-zstd` remotes

**Files:**
- Modify: `src/libostree/ostree-repo-pull.c` (lines ~970, ~1924, ~3981, ~3989, ~4517)
- Modify: `Makefile-tests.am` (script list near line 85)
- Test: `tests/test-pull-archive-zstd.sh`, `tests/test-pull-mirror-archive-zstd.sh`

**Interfaces:**
- Consumes: `_ostree_repo_mode_is_archive`, `_ostree_repo_mode_check_supported`, `_ostree_content_stream_parse_for_mode`, `_ostree_get_relative_object_path (checksum, type, mode)`.
- Produces: no new symbols.

- [ ] **Step 1: Write the failing tests**

Create `tests/test-pull-archive-zstd.sh` (mode 0755; same licence header as `tests/test-archive-zstd.sh`):

```bash
set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_ostree_feature zstd
skip_without_user_xattrs
setup_fake_remote_repo1 "archive-zstd"

repo_mode=bare-user
. ${test_srcdir}/pull-test.sh
```

Create `tests/test-pull-mirror-archive-zstd.sh` (mode 0755; same licence header):

```bash
set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_ostree_feature zstd

echo '1..5'

setup_fake_remote_repo1 "archive"
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
```

(`--http-trusted` is the `ostree pull` option that sets `OSTREE_REPO_PULL_FLAGS_TRUSTED_HTTP`; see `src/ostree/ot-builtin-pull.c:82`.)

Register both in `Makefile-tests.am` on the lines after `tests/test-pull-bareuser.sh \`:

```make
	tests/test-pull-archive-zstd.sh \
	tests/test-pull-mirror-archive-zstd.sh \
```

- [ ] **Step 2: Run them to make sure they fail**

Run: `chmod +x tests/test-pull-archive-zstd.sh tests/test-pull-mirror-archive-zstd.sh && make check TESTS="tests/test-pull-archive-zstd.sh tests/test-pull-mirror-archive-zstd.sh"`
Expected: both FAIL. `test-pull-archive-zstd.sh` fails with `Can't pull from archives with mode "archive-zstd"`. `test-pull-mirror-archive-zstd.sh` fails at the `--http-trusted` step, where zlib `.filez` bytes get stored directly as `.filezst` and `fsck` rejects them.

- [ ] **Step 3: Accept the remote mode and build the right URL**

In `src/libostree/ostree-repo-pull.c`:

Line ~4516, replace the mode check with:

```c
  if (remote_mode_loaded && pull_data->remote_repo_local == NULL
      && !_ostree_repo_mode_is_archive (pull_data->remote_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't pull from archives with mode \"%s\"", remote_mode_str);
      goto out;
    }

  if (remote_mode_loaded && pull_data->remote_repo_local == NULL
      && !_ostree_repo_mode_check_supported (pull_data->remote_mode, error))
    goto out;

  /* The fast path that stores fetched content objects without parsing them
   * is only valid when the remote uses exactly our on-disk format. */
  if (pull_data->trusted_http_direct && pull_data->remote_mode != pull_data->repo->mode)
    pull_data->trusted_http_direct = FALSE;
```

Line ~1924:

```c
      obj_subpath
          = _ostree_get_relative_object_path (expected_checksum, objtype, pull_data->remote_mode);
```

Line ~970:

```c
      if (!_ostree_content_stream_parse_for_mode (pull_data->remote_mode, tmpf_input,
                                                  stbuf.st_size, FALSE, &file_in, &file_info,
                                                  &xattrs, cancellable, error))
```

Line ~3981:

```c
      const gboolean mirroring_into_archive
          = pull_data->is_mirror && _ostree_repo_mode_is_archive (pull_data->repo->mode);
```

Line ~3989: `if (_ostree_repo_mode_is_archive (self->mode))`

- [ ] **Step 4: Verify `remote_mode` is always set before content is fetched over HTTP**

Read `ostree-repo-pull.c` from the declaration `gboolean remote_mode_loaded = FALSE;` (~line 3559) to the mode check (~line 4516) and confirm that, for a non-local remote, every path either sets `pull_data->remote_mode` from the summary or the remote `config`, or fails. Then check how `pull_data->remote_mode` is initialised (`grep -n "remote_mode" src/libostree/ostree-repo-pull.c`).

If there is a path where a non-local remote reaches object fetching with `remote_mode_loaded == FALSE`, `remote_mode` would be zero (`OSTREE_REPO_MODE_BARE`) and the new parse call would treat content as uncompressed where the old code hard-coded zlib. In that case preserve the old behaviour by adding, directly before the mode check edited in Step 3:

```c
  /* Historical default: HTTP remotes are assumed to be archive (zlib) */
  if (!remote_mode_loaded && pull_data->remote_repo_local == NULL)
    pull_data->remote_mode = OSTREE_REPO_MODE_ARCHIVE;
```

Record in the commit message which of the two situations applied.

- [ ] **Step 5: Run the tests**

Run serially (the test httpd is flaky under `-j`):
```bash
make -j"$(nproc)"
make check TESTS="tests/test-pull-archive-zstd.sh tests/test-pull-mirror-archive-zstd.sh tests/test-pull-bareuser.sh tests/test-pull-bare.sh tests/test-local-pull.sh tests/test-pull-mirror-summary.sh tests/test-pull-untrusted.sh"
```
Expected: all PASS (or SKIP for tests that need privileges this machine lacks; record which). `test-pull-archive-zstd.sh` exercises, via `pull-test.sh`, plain pulls, mirrors into `archive`, corruption detection and static-delta generation from an `archive-zstd` remote. If a `pull-test.sh` case fails only for `archive-zstd`, read its log (`cat test-pull-archive-zstd.sh.log`, or `tests/…` per the harness output), find the root cause in the code, and fix the code.

- [ ] **Step 6: Commit**

```bash
git add src/libostree/ostree-repo-pull.c tests/test-pull-archive-zstd.sh tests/test-pull-mirror-archive-zstd.sh Makefile-tests.am
git commit -m "pull: support archive-zstd remotes and cross-format mirroring"
```

---

### Task 5: Documentation and binding constants

**Files:**
- Modify: `man/ostree.repo-config.xml` (line 71; new `[archive]` section before the `[remote "name"]` section at line 266)
- Modify: `man/ostree-init.xml` (~lines 62-80)
- Modify: `docs/formats.md` (~lines 33-41)
- Modify: `src/ostree/ot-builtin-init.c:37`
- Modify: `rust-bindings/sys/src/lib.rs:116`, `rust-bindings/sys/tests/abi.rs:718`, `rust-bindings/sys/tests/constant.c:130`, `rust-bindings/src/auto/enums.rs:449-482`

**Interfaces:**
- Consumes: the names fixed in Global Constraints.
- Produces: nothing code-facing.

- [ ] **Step 1: `ostree init` help**

`src/ostree/ot-builtin-init.c:37`:

```c
          "Initialize repository in given mode (bare, bare-user, bare-user-only, archive, "
          "archive-zstd)",
          NULL },
```

- [ ] **Step 2: Man pages**

`man/ostree.repo-config.xml` line 71, replace the `mode` list item with:

```xml
        <listitem><para>One of <literal>bare</literal>, <literal>bare-user</literal>, <literal>bare-user-only</literal>, <literal>archive-z2</literal> (note that <literal>archive</literal> is used everywhere else), or <literal>archive-zstd</literal>.</para></listitem>
```

Insert a new section immediately before `<refsect1>` / `<title>[remote "name"] Section Options</title>`:

```xml
  <refsect1>
    <title>[archive] Section Options</title>

    <para>
      Options for <literal>archive</literal> and
      <literal>archive-zstd</literal> mode repositories.  They affect
      only objects written after the option is set; existing objects
      are not recompressed.
    </para>

    <variablelist>
      <varlistentry>
        <term><varname>zlib-level</varname></term>
        <listitem><para>Compression level for content objects in an
        <literal>archive</literal> repository, from <literal>1</literal>
        (fastest) to <literal>9</literal> (smallest).  Defaults to
        <literal>6</literal>.</para></listitem>
      </varlistentry>

      <varlistentry>
        <term><varname>zstd-level</varname></term>
        <listitem><para>Compression level for content objects in an
        <literal>archive-zstd</literal> repository, from
        <literal>1</literal> (fastest) to <literal>19</literal>
        (smallest).  Defaults to <literal>3</literal>.  The level does
        not noticeably affect how fast clients decompress.</para></listitem>
      </varlistentry>
    </variablelist>
  </refsect1>
```

`man/ostree-init.xml`: read lines 55-85, add `<literal>archive-zstd</literal>` to the list of valid modes, and add one sentence after the existing description of `archive`: "`archive-zstd` is like `archive` but compresses with zstd; it can only be read by versions of ostree built with zstd support."

- [ ] **Step 3: `docs/formats.md`**

After the paragraph ending "in `objects/`." (line ~41), add:

```markdown
## The archive-zstd format

`archive-zstd` is identical to `archive` except that content objects
are compressed with [zstd](https://facebook.github.io/zstd/) and stored
as `.filezst` files.  Each file is the same header used by `.filez`
followed by one standard zstd frame, so the payload can be decoded with
the `zstd` command line tool.  Object checksums are computed over the
uncompressed content, so a commit has the same checksum and signatures
in either format, and one can be converted to the other with
`ostree pull --mirror`.

zstd compresses and decompresses several times faster than zlib at a
similar or better ratio.  The cost is compatibility: a client must be
built with zstd support (`ostree --version` lists `zstd` under
`Features`), and older clients refuse `archive-zstd` remotes with an
"Invalid mode" error.  Keep an `archive` repository alongside if such
clients must be served.

The level is set with `zstd-level` in the `[archive]` section of the
repository config (1-19, default 3).
```

- [ ] **Step 4: Rust binding constants**

Follow exactly how `OSTREE_REPO_MODE_BARE_SPLIT_XATTRS` appears in each file:

`rust-bindings/sys/src/lib.rs`, after line 116:

```rust
pub const OSTREE_REPO_MODE_ARCHIVE_ZSTD: OstreeRepoMode = 5;
```

`rust-bindings/sys/tests/abi.rs`, keeping the list's alphabetical order (insert after the `OSTREE_REPO_MODE_ARCHIVE` entry):

```rust
    ("(gint) OSTREE_REPO_MODE_ARCHIVE_ZSTD", "5"),
```

`rust-bindings/sys/tests/constant.c`, same position:

```c
    PRINT_CONSTANT((gint) OSTREE_REPO_MODE_ARCHIVE_ZSTD);
```

`rust-bindings/src/auto/enums.rs`: read lines 435-490 and add an `ArchiveZstd` variant with `#[doc(alias = "OSTREE_REPO_MODE_ARCHIVE_ZSTD")]` after `BareSplitXattrs`, plus the matching arms in `into_glib` (`Self::ArchiveZstd => ffi::OSTREE_REPO_MODE_ARCHIVE_ZSTD,`) and `from_glib` (`ffi::OSTREE_REPO_MODE_ARCHIVE_ZSTD => Self::ArchiveZstd,`), and any `Display`/`fmt` arm the file has for the other variants. If the neighbouring variants carry `#[cfg(feature = "v20xx_x")]` gates, do not invent a new feature flag: leave `enums.rs` unchanged and note in the commit message that the high-level binding will pick the variant up on the next GIR regeneration.

- [ ] **Step 5: Verify**

Run:
```bash
make -j"$(nproc)"
./ostree --help >/dev/null; ./ostree init --help | grep archive-zstd
xmllint --noout man/ostree.repo-config.xml man/ostree-init.xml
(cd rust-bindings && cargo check -p ostree-sys 2>&1 | tail -3) || echo "cargo unavailable: skipped"
```
Expected: help text shows `archive-zstd`; `xmllint` prints nothing. If `xmllint` or `cargo` is not in the dev shell, say so in the report rather than claiming it passed.

- [ ] **Step 6: Commit**

```bash
git add man docs/formats.md src/ostree/ot-builtin-init.c rust-bindings
git commit -m "docs: document archive-zstd mode and the [archive] config section"
```

---

### Task 6: No-zstd build, regression sweep and benchmark

**Files:**
- Modify: `docs/superpowers/specs/2026-10-09-archive-zstd-design.md` (record benchmark result; adjust default only if the user agrees)
- No product code unless a regression is found.

**Interfaces:**
- Consumes: everything above.
- Produces: verification evidence and benchmark numbers for the final report.

- [ ] **Step 1: Build without zstd in a separate tree**

```bash
git worktree list   # confirm we are in the archive-zstd worktree
mkdir -p /home/blake/.claude/jobs/41ab5e27/tmp/build-nozstd
cd /home/blake/.claude/jobs/41ab5e27/tmp/build-nozstd
/home/blake/dev/ostree-fork/ostree/.claude/worktrees/archive-zstd/configure --with-curl --without-zstd
make -j"$(nproc)"
```

If out-of-tree builds do not work for this project, instead run `make distclean` in the worktree, configure `--without-zstd` there, and restore the `--with-zstd` configuration afterwards.

Expected: configure summary shows `zstd (archive-zstd repositories):             no`; `features:` has no `zstd`; the build succeeds with no reference to `zstd.h`.

- [ ] **Step 2: Check the no-zstd behaviour**

```bash
./ostree --version | grep -c zstd || true            # expect 0
./ostree --repo=/home/blake/.claude/jobs/41ab5e27/tmp/nozstd-repo init --mode=archive-zstd; echo "exit=$?"
```
Expected: `error: This build of ostree does not support archive-zstd repositories`, non-zero exit, and no `config` file created in the target directory.

Then, using a repository created by the zstd-enabled build:
```bash
/home/blake/dev/ostree-fork/ostree/.claude/worktrees/archive-zstd/ostree --repo=/home/blake/.claude/jobs/41ab5e27/tmp/zstd-repo init --mode=archive-zstd
./ostree --repo=/home/blake/.claude/jobs/41ab5e27/tmp/zstd-repo refs; echo "exit=$?"
```
Expected: the same error, non-zero exit.

Run the archive regression tests in this build: `make check TESTS="tests/test-archivez.sh tests/test-basic-c tests/test-pull-bareuser.sh"`. Expected: PASS, and `tests/test-archive-zstd.sh` (if selected) reports SKIP.

- [ ] **Step 3: Full regression in the zstd build**

Back in the worktree:
```bash
make check -j"$(nproc)" 2>&1 | tail -40
```
Re-run every failure serially with `make check TESTS="<name>"` before believing it. Compare the final failure set against the known six (`test-core.js`, `test-pull-corruption.sh`, `test-remotes-config-dir.js`, `test-sizes.js`, `test-symbols.sh`, `test-sysroot.js`). `test-pull-corruption.sh` and `test-sizes.js` touch archive objects: for each of those two, confirm it also fails on the base commit `fdef7e6` (e.g. via `git stash`-free comparison: build `fdef7e6` in a second out-of-tree directory, or read the log to confirm the failure is environmental) before attributing it to the environment. Any new failure is a regression: debug and fix it in the task that owns the code, with its own commit.

- [ ] **Step 4: Benchmark**

Primary data set: the user's real sol3 OS tree, in the local bare-user repository `~/.cache/space-ng/sol3-ostree-local` (ref `sysroot/version/0.3.1`; list refs with `ostree --repo=... refs` and pick that or the largest available). Treat that repository as strictly read-only: only ever name it as the *source* of `pull-local`. For each configuration, create a scratch repository under `$B`, set the level, and time

```bash
/usr/bin/time -f "$name pull-local: %e s wall, %U s user" \
  $OSTREE --repo=$B/repo-$name pull-local --disable-fsync ~/.cache/space-ng/sol3-ostree-local sysroot/version/0.3.1
```

then record `du -sb $B/repo-$name/objects` and a timed `checkout -U`. `pull-local` from a bare-user source into an archive repository reads plain files and compresses them, so it measures exactly the compose-side cost. If that repository is missing or unreadable, fall back to the nix closure below, which is a few hundred MB of binaries, libraries and text:

```bash
B=/home/blake/.claude/jobs/41ab5e27/tmp/bench
mkdir -p $B/tree
cp -a --no-preserve=ownership $(nix build --no-link --print-out-paths nixpkgs#systemd nixpkgs#glibc nixpkgs#python3 nixpkgs#coreutils) $B/tree/
chmod -R u+w $B/tree
du -sh $B/tree
OSTREE=/home/blake/dev/ostree-fork/ostree/.claude/worktrees/archive-zstd/ostree
bench() {  # name mode key level
  rm -rf $B/repo-$1
  $OSTREE --repo=$B/repo-$1 init --mode=$2
  [ -n "$3" ] && $OSTREE --repo=$B/repo-$1 config set archive.$3 $4
  /usr/bin/time -f "$1 commit: %e s wall, %U s user" $OSTREE --repo=$B/repo-$1 commit -b bench --tree=dir=$B/tree --owner-uid=0 --owner-gid=0 --no-xattrs >/dev/null
  echo "$1 size: $(du -sb $B/repo-$1/objects | cut -f1)"
  rm -rf $B/co
  /usr/bin/time -f "$1 checkout: %e s wall" $OSTREE --repo=$B/repo-$1 checkout -U --disable-cache bench $B/co  2>&1 || /usr/bin/time -f "$1 checkout: %e s wall" $OSTREE --repo=$B/repo-$1 checkout -U bench $B/co
}
bench zlib6 archive "" ""
bench zstd1 archive-zstd zstd-level 1
bench zstd3 archive-zstd zstd-level 3
bench zstd9 archive-zstd zstd-level 9
bench zstd12 archive-zstd zstd-level 12
bench zstd19 archive-zstd zstd-level 19
```

Check `ostree checkout --help` for the real name of the option that disables the uncompressed-object cache and use it; the `||` fallback is only there so a wrong option name does not abort the run. Run the whole set twice and report the second run (warm page cache). Verify all repos agree on the commit's root content: `for r in $B/repo-*; do $OSTREE --repo=$r ls -C bench / | head -1; done` must print the same checksums.

- [ ] **Step 5: Record the result**

Add a section `## 11. Benchmark` to the spec with a table (tree size, then per configuration: repo size in bytes and as a percentage of zlib 6, commit wall time, checkout wall time), the machine's core count, and the libzstd version. State plainly whether level 3 is at least as small as zlib 6. If it is not, or if a mid level is close to free, do NOT change the default: put the recommendation in the final report for the user to decide.

- [ ] **Step 6: Commit and push**

```bash
git add docs/superpowers/specs/2026-10-09-archive-zstd-design.md
git commit -m "docs: record archive-zstd benchmark results"
git push fork worktree-archive-zstd:archive-zstd
```

Push only to `fork` (`xyven1/ostree`). Do not open a PR unless the user asks.
