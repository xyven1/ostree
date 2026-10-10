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
#include <zstd.h>

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
  const guint8 *orig = g_bytes_get_data (compressed, &len);
  g_autofree guint8 *copy = g_memdup2 (orig, len);
  /* Byte 4 is the frame header descriptor */
  copy[4] ^= 0xff;
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

/* Compress @data with an explicit window log, as a frame of unknown content
 * size so that the frame header carries the window descriptor. */
static GBytes *
compress_with_window_log (const guint8 *data, gsize data_size, int window_log)
{
  ZSTD_CCtx *cctx = ZSTD_createCCtx ();
  g_assert_nonnull (cctx);
  g_assert_false (ZSTD_isError (ZSTD_CCtx_setParameter (cctx, ZSTD_c_windowLog, window_log)));

  const gsize out_size = ZSTD_compressBound (data_size) + 64;
  guint8 *out_data = g_malloc (out_size);
  ZSTD_inBuffer in = { data, data_size, 0 };
  ZSTD_outBuffer out = { out_data, out_size, 0 };

  /* A first call with ZSTD_e_end would make this a single-segment frame of
   * known size, with no window descriptor; flush first. */
  g_assert_false (ZSTD_isError (ZSTD_compressStream2 (cctx, &out, &in, ZSTD_e_flush)));
  g_assert_cmpint (ZSTD_compressStream2 (cctx, &out, &in, ZSTD_e_end), ==, 0);
  ZSTD_freeCCtx (cctx);

  return g_bytes_new_take (out_data, out.pos);
}

/* Frames needing more window memory than ostree itself ever requests must be
 * refused, since objects can come from an untrusted remote. */
static void
test_zstd_window_limit (void)
{
  const gsize buffer_size = 64 * 1024;
  g_autofree guint8 *buffer = g_malloc (buffer_size);
  memset (buffer, 'w', buffer_size);

  {
    /* 8 MiB window: the largest the levels ostree allows (1-19) produce */
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) compressed = compress_with_window_log (buffer, buffer_size, 23);
    g_autoptr (GBytes) result = decompress_bytes (compressed, &error);
    g_assert_no_error (error);
    g_assert_cmpmem (g_bytes_get_data (result, NULL), g_bytes_get_size (result), buffer,
                     buffer_size);
  }

  {
    /* 16 MiB window: one step over */
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) compressed = compress_with_window_log (buffer, buffer_size, 24);
    g_autoptr (GBytes) result = decompress_bytes (compressed, &error);
    g_assert_null (result);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  }
}

/* The highest level ostree lets a repository be configured with must stay
 * within the window the decompressor accepts, whatever the input size. */
static void
test_zstd_max_level_within_window_limit (void)
{
  const gsize buffer_size = 20 * 1024 * 1024;
  g_autofree guint8 *buffer = g_malloc (buffer_size);
  g_autoptr (GRand) r = g_rand_new_with_seed (17);
  for (gsize i = 0; i < buffer_size; i++)
    buffer[i] = g_rand_int_range (r, 0, 4);

  assert_roundtrip (buffer, buffer_size, 19);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/zstd/window-limit", test_zstd_window_limit);
  g_test_add_func ("/zstd/max-level-within-window-limit", test_zstd_max_level_within_window_limit);
  g_test_add_func ("/zstd/empty", test_zstd_empty);
  g_test_add_func ("/zstd/random-buffer", test_zstd_random);
  g_test_add_func ("/zstd/big-buffers", test_zstd_big_buffers);
  g_test_add_func ("/zstd/levels", test_zstd_levels);
  g_test_add_func ("/zstd/truncated", test_zstd_truncated);
  g_test_add_func ("/zstd/garbage", test_zstd_garbage);
  g_test_add_func ("/zstd/corrupted", test_zstd_corrupted);

  return g_test_run ();
}
