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

/* Streaming, optionally multi-threaded compression for stream delta parts.
 * Unlike the GConverter based compressors this never needs the whole input
 * or output in memory, and lets the caller pick the speed/size tradeoff.
 */

typedef struct
{
  /* An OSTREE_STATIC_DELTA_COMPRESSION_* value */
  guint8 type;
  /* zstd level, or xz preset (0-9) */
  int level;
  /* xz only: use the "extreme" preset variant */
  gboolean extreme;
  /* log2 of the zstd window, or of the xz dictionary size; 0 for the default
   * of the level.  This bounds the memory needed to decompress.
   */
  guint window_log;
  /* Worker threads; 0 or 1 compresses on the calling thread.  For xz this
   * also splits the input into independent blocks, costing some size.
   */
  guint threads;
} OtDeltaCompressOptions;

/* Read up to @len bytes; return 0 at end of input, -1 on error. */
typedef gssize (*OtDeltaReadFunc) (gpointer user_data, guint8 *buf, gsize len, GError **error);
typedef gboolean (*OtDeltaWriteFunc) (gpointer user_data, const guint8 *buf, gsize len,
                                      GError **error);

gboolean _ostree_delta_compress (const OtDeltaCompressOptions *options, OtDeltaReadFunc read,
                                 gpointer read_data, OtDeltaWriteFunc write, gpointer write_data,
                                 GCancellable *cancellable, GError **error);

typedef struct OtDeltaDecompressor OtDeltaDecompressor;

/* @data must stay valid for the lifetime of the decompressor. */
OtDeltaDecompressor *_ostree_delta_decompressor_new (guint8 type, const guint8 *data, gsize len,
                                                     GError **error);
/* Fill @buf completely unless the end of the stream is reached first. */
gboolean _ostree_delta_decompressor_read (OtDeltaDecompressor *self, guint8 *buf, gsize len,
                                          gsize *out_bytes_read, GError **error);
void _ostree_delta_decompressor_free (OtDeltaDecompressor *self);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OtDeltaDecompressor, _ostree_delta_decompressor_free)

G_END_DECLS
