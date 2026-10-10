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
                       "Invalid zstd compression level %d: %s", self->level, ZSTD_getErrorName (r));
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
