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
#include "ostree-hdiff.h"
#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#define OLD_SIZE (512 * 1024)
#define NEW_SIZE (512 * 1024 + 1000)

static void
fill (guint8 *buf, gsize len, guint32 seed)
{
  g_autoptr (GRand) rand = g_rand_new_with_seed (seed);
  for (gsize i = 0; i < len; i++)
    buf[i] = (guint8)g_rand_int_range (rand, 0, 256);
}

/* New data: old data with a small insertion and a few scattered edits */
static void
make_pair (guint8 **out_old, guint8 **out_new)
{
  guint8 *old = g_malloc (OLD_SIZE);
  guint8 *new = g_malloc (NEW_SIZE);
  fill (old, OLD_SIZE, 42);
  memcpy (new, old, 100000);
  fill (new + 100000, 1000, 7);
  memcpy (new + 101000, old + 100000, OLD_SIZE - 100000);
  for (gsize i = 0; i < NEW_SIZE; i += 50021)
    new[i] ^= 0x5a;
  *out_old = old;
  *out_new = new;
}

typedef struct
{
  const guint8 *data;
  gsize len;
  guint64 next_pos;
  gboolean sequential;
} MemInput;

static gboolean
mem_read (gpointer user_data, guint64 pos, guint8 *buf, gsize len)
{
  MemInput *in = user_data;
  if (pos + len > in->len)
    return FALSE;
  if (in->sequential)
    g_assert_cmpuint (pos, ==, in->next_pos);
  memcpy (buf, in->data + pos, len);
  in->next_pos = pos + len;
  return TRUE;
}

static gboolean
bytearray_write (gpointer user_data, const guint8 *buf, gsize len)
{
  g_byte_array_append (user_data, buf, len);
  return TRUE;
}

/* Diff two buffers with the precise differ */
static gboolean
create_mem (const guint8 *old, gsize old_len, const guint8 *new, gsize new_len,
            const OtHdiffMemOptions *opts, GBytes **out_diff, GError **error)
{
  MemInput old_mem = { old, old_len, 0, FALSE };
  MemInput new_mem = { new, new_len, 0, FALSE };
  OtHdiffInput old_in = { old_len, mem_read, &old_mem };
  OtHdiffInput new_in = { new_len, mem_read, &new_mem };
  return _ostree_hdiff_create_block (&old_in, &new_in, opts, out_diff, error);
}

/* Patch, checking that the diff is read strictly sequentially */
static gboolean
patch_mem (const guint8 *old, gsize old_len, const guint8 *diff, gsize diff_len, GByteArray *out,
           gsize new_len, GError **error)
{
  MemInput old_mem = { old, old_len, 0, FALSE };
  MemInput diff_mem = { diff, diff_len, 0, TRUE };
  OtHdiffInput old_in = { old_len, mem_read, &old_mem };
  OtHdiffInput diff_in = { diff_len, mem_read, &diff_mem };
  g_byte_array_set_size (out, 0);
  return _ostree_hdiff_patch_stream (&old_in, &diff_in, new_len, bytearray_write, out, error);
}

static void
test_mem_roundtrip (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree guint8 *old = NULL;
  g_autofree guint8 *new = NULL;
  make_pair (&old, &new);

  for (guint variant = 0; variant < 2; variant++)
    {
      OtHdiffMemOptions opts = OT_HDIFF_MEM_OPTIONS_DEFAULT;
      if (variant == 1)
        opts.fast_match_block = 64 * 1024;
      g_autoptr (GBytes) diff = NULL;
      create_mem (old, OLD_SIZE, new, NEW_SIZE, &opts, &diff, &error);
      g_assert_no_error (error);
      /* Nearly everything is shared with the old data */
      g_assert_cmpuint (g_bytes_get_size (diff), <, 8192);

      g_autoptr (GByteArray) out = g_byte_array_new ();
      patch_mem (old, OLD_SIZE, g_bytes_get_data (diff, NULL), g_bytes_get_size (diff), out,
                 NEW_SIZE, &error);
      g_assert_no_error (error);
      g_assert_cmpmem (out->data, out->len, new, NEW_SIZE);
    }
}

static void
test_stream_roundtrip (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree guint8 *old = NULL;
  g_autofree guint8 *new = NULL;
  make_pair (&old, &new);

  g_auto (GLnxTmpfile) tmpf = {
    0,
  };
  glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &tmpf, &error);
  g_assert_no_error (error);

  MemInput old_mem = { old, OLD_SIZE, 0, FALSE };
  MemInput new_mem = { new, NEW_SIZE, 0, FALSE };
  OtHdiffInput old_in = { OLD_SIZE, mem_read, &old_mem };
  OtHdiffInput new_in = { NEW_SIZE, mem_read, &new_mem };
  guint64 diff_size = 0;
  _ostree_hdiff_create_stream (&old_in, &new_in, tmpf.fd, 64, 2, &diff_size, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (diff_size, >, 0);
  g_assert_cmpuint (diff_size, <, 64 * 1024);

  g_autoptr (GBytes) diff = glnx_fd_readall_bytes (tmpf.fd, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_bytes_get_size (diff), ==, diff_size);

  g_autoptr (GByteArray) out = g_byte_array_new ();
  patch_mem (old, OLD_SIZE, g_bytes_get_data (diff, NULL), diff_size, out, NEW_SIZE, &error);
  g_assert_no_error (error);
  g_assert_cmpmem (out->data, out->len, new, NEW_SIZE);
}

static void
test_empty_old (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree guint8 *new = g_malloc (4096);
  fill (new, 4096, 3);
  g_autoptr (GBytes) diff = NULL;
  create_mem ((const guint8 *)"", 0, new, 4096, NULL, &diff, &error);
  g_assert_no_error (error);
  g_autoptr (GByteArray) out = g_byte_array_new ();
  patch_mem ((const guint8 *)"", 0, g_bytes_get_data (diff, NULL), g_bytes_get_size (diff), out,
             4096, &error);
  g_assert_no_error (error);
  g_assert_cmpmem (out->data, out->len, new, 4096);
}

static void
test_bad_input (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree guint8 *old = NULL;
  g_autofree guint8 *new = NULL;
  make_pair (&old, &new);
  g_autoptr (GBytes) diff = NULL;
  create_mem (old, OLD_SIZE, new, NEW_SIZE, NULL, &diff, &error);
  g_assert_no_error (error);
  gsize diff_len = g_bytes_get_size (diff);
  const guint8 *diff_data = g_bytes_get_data (diff, NULL);
  g_autoptr (GByteArray) out = g_byte_array_new ();

  /* Wrong target size */
  g_assert_false (patch_mem (old, OLD_SIZE, diff_data, diff_len, out, NEW_SIZE - 1, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  /* Wrong source size */
  g_assert_false (patch_mem (old, OLD_SIZE - 1, diff_data, diff_len, out, NEW_SIZE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  /* Truncated diff */
  g_assert_false (patch_mem (old, OLD_SIZE, diff_data, diff_len / 2, out, NEW_SIZE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  /* Not a diff at all */
  g_assert_false (patch_mem (old, OLD_SIZE, old, 4096, out, NEW_SIZE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/hdiff/mem-roundtrip", test_mem_roundtrip);
  g_test_add_func ("/hdiff/stream-roundtrip", test_stream_roundtrip);
  g_test_add_func ("/hdiff/empty-old", test_empty_old);
  g_test_add_func ("/hdiff/bad-input", test_bad_input);
  return g_test_run ();
}
