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

#include "ostree-zstd-decompressor.h"

#include <zstd.h>

/**
 * SECTION:ostree-zstd-decompressor
 * @title: zstd decompressor
 *
 * An implementation of #GConverter that decompresses a single zstd frame.
 *
 */

/* Refuse frames that need more than an 8 MiB window.  Content objects can come
 * from an untrusted remote and several are decoded at once, so without a cap a
 * tiny crafted frame could make each decoder allocate libzstd's default limit
 * of 128 MiB.  8 MiB is the largest window produced by the compression levels
 * an archive-zstd repository can be configured with (1-19).  This is part of
 * the archive-zstd format; see docs/formats.md.
 */
#define OSTREE_ZSTD_DECODER_WINDOWLOG_MAX 23

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
      size_t r = ZSTD_DCtx_setParameter (self->dctx, ZSTD_d_windowLogMax,
                                         OSTREE_ZSTD_DECODER_WINDOWLOG_MAX);
      if (ZSTD_isError (r))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to set zstd window limit: %s",
                       ZSTD_getErrorName (r));
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
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT, "Truncated zstd stream");
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
