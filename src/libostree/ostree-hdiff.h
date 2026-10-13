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

/* A thin C interface over HDiffPatch (https://github.com/sisong/HDiffPatch).
 *
 * All diffs produced here are *uncompressed* "single compressed diff"
 * (HDIFFSF20) streams; callers apply their own compression on top.  That
 * format is read strictly sequentially when patching, the old data is read
 * by random access, and the new data is written strictly sequentially.
 */

/* Random-access input.  @read must fill exactly @len bytes starting at @pos,
 * and may be invoked from multiple threads, but never concurrently.
 */
typedef struct
{
  guint64 size;
  gboolean (*read) (gpointer user_data, guint64 pos, guint8 *buf, gsize len);
  gpointer user_data;
} OtHdiffInput;

/* Sequential output. */
typedef gboolean (*OtHdiffWrite) (gpointer user_data, const guint8 *buf, gsize len);

typedef struct
{
  /* Size of the blocks used to quickly skip over identical regions before
   * the (much slower) suffix-array match of what is left.  Larger blocks
   * leave more for the slow match: slower, slightly smaller diffs.
   */
  gsize fast_match_block;
  /* Minimum score for a match to be used; lower finds more, smaller matches. */
  int match_score;
  /* Trade memory (O(old size)) for speed in the suffix-array match. */
  gboolean big_cache;
  guint threads;
  /* Emit HDiffPatch's older "compressed diff" format (HDIFF13) rather than
   * the single-stream one.  Only for comparing the two; patching it needs the
   * whole diff in memory.
   */
  gboolean legacy_format;
} OtHdiffMemOptions;

#define OT_HDIFF_MEM_OPTIONS_DEFAULT { 1024, 6, FALSE, 1, FALSE }

/* Precise differ: loads what it needs of both inputs into memory (in the
 * worst case all of both, plus working space of several times the old size).
 */
gboolean _ostree_hdiff_create_block (const OtHdiffInput *old_in, const OtHdiffInput *new_in,
                                     const OtHdiffMemOptions *options, GBytes **out_diff,
                                     GError **error);

gboolean _ostree_hdiff_create_mem (const guint8 *old_data, gsize old_len, const guint8 *new_data,
                                   gsize new_len, const OtHdiffMemOptions *options,
                                   GBytes **out_diff, GError **error);

/* Low-memory differ: matches @match_block sized blocks of @new_in against
 * @old_in.  Much faster than the in-memory differ on dissimilar data, but
 * far less precise on similar data.  @out_fd must be seekable.
 */
gboolean _ostree_hdiff_create_stream (const OtHdiffInput *old_in, const OtHdiffInput *new_in,
                                      int out_fd, gsize match_block, guint threads,
                                      guint64 *out_diff_size, GError **error);

gboolean _ostree_hdiff_patch_mem (const guint8 *old_data, gsize old_len, const guint8 *diff,
                                  gsize diff_len, guint8 *out_new, gsize new_len, GError **error);

/* @diff_in is read sequentially; @expected_new_size must match the diff. */
gboolean _ostree_hdiff_patch_stream (const OtHdiffInput *old_in, const OtHdiffInput *diff_in,
                                     guint64 expected_new_size, OtHdiffWrite write,
                                     gpointer write_data, GError **error);

G_END_DECLS
