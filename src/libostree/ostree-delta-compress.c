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

#include <lzma.h>
#include <string.h>
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

#include "ostree-delta-compress.h"
#include "ostree-repo-static-delta-private.h"
#include "otutil.h"

#define IO_BUF_SIZE (1024 * 1024)

/* Largest window/dictionary we will ever decode: bounds decompression memory
 * for untrusted input.  The generator never exceeds it (128 MiB).
 */
#define MAX_WINDOW_LOG 27
/* An LZMA2 decoder needs its dictionary plus well under a megabyte */
#define XZ_DECODER_MEMLIMIT ((G_GUINT64_CONSTANT (1) << MAX_WINDOW_LOG) + (16 << 20))

static gboolean
throw_lzma (lzma_ret r, const char *what, GError **error)
{
  return glnx_throw (error, "xz %s failed (liblzma error %d)", what, (int)r);
}

static gboolean
compress_none (OtDeltaReadFunc read, gpointer read_data, OtDeltaWriteFunc write,
               gpointer write_data, GCancellable *cancellable, GError **error)
{
  g_autofree guint8 *buf = g_malloc (IO_BUF_SIZE);
  while (TRUE)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;
      gssize n = read (read_data, buf, IO_BUF_SIZE, error);
      if (n < 0)
        return FALSE;
      if (n == 0)
        return TRUE;
      if (!write (write_data, buf, n, error))
        return FALSE;
    }
}

static gboolean
compress_xz (const OtDeltaCompressOptions *options, OtDeltaReadFunc read, gpointer read_data,
             OtDeltaWriteFunc write, gpointer write_data, GCancellable *cancellable, GError **error)
{
  lzma_stream strm = LZMA_STREAM_INIT;
  lzma_options_lzma lzma_opts;
  const uint32_t preset = (uint32_t)CLAMP (options->level, 0, 9);
  if (lzma_lzma_preset (&lzma_opts, preset))
    return glnx_throw (error, "Invalid xz preset %d", options->level);
  if (options->window_log > 0)
    lzma_opts.dict_size = ((uint32_t)1) << CLAMP (options->window_log, 12, MAX_WINDOW_LOG);
  /* The decoder allocates the whole dictionary, so a small input should not
   * declare a large one.  (zstd does this itself given the input size.)
   */
  if (options->src_size_known)
    {
      uint32_t fit = 1 << 12;
      while (fit < options->src_size && fit < lzma_opts.dict_size)
        fit <<= 1;
      lzma_opts.dict_size = MIN (lzma_opts.dict_size, fit);
    }

  lzma_filter filters[] = { { .id = LZMA_FILTER_LZMA2, .options = &lzma_opts },
                            { .id = LZMA_VLI_UNKNOWN, .options = NULL } };

  lzma_ret r;
  if (options->threads > 1)
    {
      lzma_mt mt = {
        .threads = options->threads,
        /* 0: liblzma picks three times the dictionary size */
        .block_size = 0,
        .filters = filters,
        .check = LZMA_CHECK_CRC32,
      };
      r = lzma_stream_encoder_mt (&strm, &mt);
    }
  else
    r = lzma_stream_encoder (&strm, filters, LZMA_CHECK_CRC32);
  if (r != LZMA_OK)
    return throw_lzma (r, "encoder setup", error);

  gboolean ret = FALSE;
  g_autofree guint8 *inbuf = g_malloc (IO_BUF_SIZE);
  g_autofree guint8 *outbuf = g_malloc (IO_BUF_SIZE);
  lzma_action action = LZMA_RUN;
  strm.next_out = outbuf;
  strm.avail_out = IO_BUF_SIZE;
  while (TRUE)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;
      if (strm.avail_in == 0 && action == LZMA_RUN)
        {
          gssize n = read (read_data, inbuf, IO_BUF_SIZE, error);
          if (n < 0)
            goto out;
          if (n == 0)
            action = LZMA_FINISH;
          strm.next_in = inbuf;
          strm.avail_in = (size_t)n;
        }
      r = lzma_code (&strm, action);
      if (strm.avail_out == 0 || r == LZMA_STREAM_END)
        {
          if (!write (write_data, outbuf, IO_BUF_SIZE - strm.avail_out, error))
            goto out;
          strm.next_out = outbuf;
          strm.avail_out = IO_BUF_SIZE;
        }
      if (r == LZMA_STREAM_END)
        break;
      if (r != LZMA_OK)
        {
          throw_lzma (r, "compression", error);
          goto out;
        }
    }
  ret = TRUE;
out:
  lzma_end (&strm);
  return ret;
}

#ifdef HAVE_ZSTD
static gboolean
compress_zstd (const OtDeltaCompressOptions *options, OtDeltaReadFunc read, gpointer read_data,
               OtDeltaWriteFunc write, gpointer write_data, GCancellable *cancellable,
               GError **error)
{
  ZSTD_CCtx *cctx = ZSTD_createCCtx ();
  if (cctx == NULL)
    return glnx_throw (error, "Failed to allocate zstd context");

  gboolean ret = FALSE;
  const size_t outcap = ZSTD_CStreamOutSize ();
  g_autofree guint8 *inbuf = g_malloc (IO_BUF_SIZE);
  g_autofree guint8 *outbuf = g_malloc (outcap);

  size_t zr = ZSTD_CCtx_setParameter (cctx, ZSTD_c_compressionLevel, options->level);
  if (ZSTD_isError (zr))
    {
      glnx_throw (error, "zstd: %s", ZSTD_getErrorName (zr));
      goto out;
    }
  if (options->window_log > 0)
    {
      const int wlog = CLAMP ((int)options->window_log, 10, MAX_WINDOW_LOG);
      zr = ZSTD_CCtx_setParameter (cctx, ZSTD_c_windowLog, wlog);
      if (ZSTD_isError (zr))
        {
          glnx_throw (error, "zstd: %s", ZSTD_getErrorName (zr));
          goto out;
        }
      /* Large windows only pay off with long distance matching */
      if (wlog >= 25)
        (void)ZSTD_CCtx_setParameter (cctx, ZSTD_c_enableLongDistanceMatching, 1);
    }
  if (options->src_size_known)
    {
      zr = ZSTD_CCtx_setPledgedSrcSize (cctx, options->src_size);
      if (ZSTD_isError (zr))
        {
          glnx_throw (error, "zstd: %s", ZSTD_getErrorName (zr));
          goto out;
        }
    }
  /* Fails harmlessly if libzstd was built without threading */
  if (options->threads > 1)
    (void)ZSTD_CCtx_setParameter (cctx, ZSTD_c_nbWorkers, (int)options->threads);

  gboolean eof = FALSE;
  while (!eof)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;
      gssize n = read (read_data, inbuf, IO_BUF_SIZE, error);
      if (n < 0)
        goto out;
      eof = (n == 0);
      ZSTD_inBuffer in = { inbuf, (size_t)n, 0 };
      const ZSTD_EndDirective mode = eof ? ZSTD_e_end : ZSTD_e_continue;
      gboolean finished = FALSE;
      while (!finished)
        {
          ZSTD_outBuffer outb = { outbuf, outcap, 0 };
          zr = ZSTD_compressStream2 (cctx, &outb, &in, mode);
          if (ZSTD_isError (zr))
            {
              glnx_throw (error, "zstd compression failed: %s", ZSTD_getErrorName (zr));
              goto out;
            }
          if (outb.pos > 0 && !write (write_data, outbuf, outb.pos, error))
            goto out;
          finished = eof ? (zr == 0) : (in.pos == in.size);
        }
    }
  ret = TRUE;
out:
  ZSTD_freeCCtx (cctx);
  return ret;
}
#endif

gboolean
_ostree_delta_compress (const OtDeltaCompressOptions *options, OtDeltaReadFunc read,
                        gpointer read_data, OtDeltaWriteFunc write, gpointer write_data,
                        GCancellable *cancellable, GError **error)
{
  switch (options->type)
    {
    case OSTREE_STATIC_DELTA_COMPRESSION_NONE:
      return compress_none (read, read_data, write, write_data, cancellable, error);
    case OSTREE_STATIC_DELTA_COMPRESSION_XZ:
      return compress_xz (options, read, read_data, write, write_data, cancellable, error);
    case OSTREE_STATIC_DELTA_COMPRESSION_ZSTD:
#ifdef HAVE_ZSTD
      return compress_zstd (options, read, read_data, write, write_data, cancellable, error);
#else
      return glnx_throw (error, "This version of ostree was built without zstd support");
#endif
    default:
      return glnx_throw (error, "Invalid compression type '%u'", options->type);
    }
}

struct OtDeltaDecompressor
{
  guint8 type;
  const guint8 *data;
  gsize len;
  gsize pos;
  gboolean finished;
  lzma_stream lzma;
  gboolean lzma_initialized;
#ifdef HAVE_ZSTD
  ZSTD_DCtx *dctx;
#endif
};

void
_ostree_delta_decompressor_free (OtDeltaDecompressor *self)
{
  if (self == NULL)
    return;
  if (self->lzma_initialized)
    lzma_end (&self->lzma);
#ifdef HAVE_ZSTD
  if (self->dctx)
    ZSTD_freeDCtx (self->dctx);
#endif
  g_free (self);
}

OtDeltaDecompressor *
_ostree_delta_decompressor_new (guint8 type, const guint8 *data, gsize len, GError **error)
{
  g_autoptr (OtDeltaDecompressor) self = g_new0 (OtDeltaDecompressor, 1);
  self->type = type;
  self->data = data;
  self->len = len;

  switch (type)
    {
    case OSTREE_STATIC_DELTA_COMPRESSION_NONE:
      break;
    case OSTREE_STATIC_DELTA_COMPRESSION_XZ:
      {
        lzma_stream init = LZMA_STREAM_INIT;
        self->lzma = init;
        lzma_ret r = lzma_stream_decoder (&self->lzma, XZ_DECODER_MEMLIMIT, 0);
        if (r != LZMA_OK)
          return throw_lzma (r, "decoder setup", error), NULL;
        self->lzma_initialized = TRUE;
      }
      break;
    case OSTREE_STATIC_DELTA_COMPRESSION_ZSTD:
#ifdef HAVE_ZSTD
      self->dctx = ZSTD_createDCtx ();
      if (self->dctx == NULL)
        return glnx_null_throw (error, "Failed to allocate zstd context");
      (void)ZSTD_DCtx_setParameter (self->dctx, ZSTD_d_windowLogMax, MAX_WINDOW_LOG);
      break;
#else
      return glnx_null_throw (error, "Static delta part is zstd-compressed, but this version of "
                                     "ostree was built without zstd support");
#endif
    default:
      return glnx_null_throw (error, "Invalid compression type '%u'", type);
    }

  return g_steal_pointer (&self);
}

gboolean
_ostree_delta_decompressor_read (OtDeltaDecompressor *self, guint8 *buf, gsize len,
                                 gsize *out_bytes_read, GError **error)
{
  gsize done = 0;

  switch (self->type)
    {
    case OSTREE_STATIC_DELTA_COMPRESSION_NONE:
      done = MIN (len, self->len - self->pos);
      memcpy (buf, self->data + self->pos, done);
      self->pos += done;
      break;
    case OSTREE_STATIC_DELTA_COMPRESSION_XZ:
      while (done < len && !self->finished)
        {
          self->lzma.next_in = self->data + self->pos;
          self->lzma.avail_in = self->len - self->pos;
          self->lzma.next_out = buf + done;
          self->lzma.avail_out = len - done;
          lzma_ret r = lzma_code (&self->lzma, self->lzma.avail_in == 0 ? LZMA_FINISH : LZMA_RUN);
          self->pos = self->len - self->lzma.avail_in;
          done = len - self->lzma.avail_out;
          if (r == LZMA_STREAM_END)
            self->finished = TRUE;
          else if (r != LZMA_OK)
            return throw_lzma (r, "decompression", error);
        }
      break;
#ifdef HAVE_ZSTD
    case OSTREE_STATIC_DELTA_COMPRESSION_ZSTD:
      while (done < len && !self->finished)
        {
          ZSTD_inBuffer in = { self->data, self->len, self->pos };
          ZSTD_outBuffer outb = { buf, len, done };
          size_t zr = ZSTD_decompressStream (self->dctx, &outb, &in);
          if (ZSTD_isError (zr))
            return glnx_throw (error, "zstd decompression failed: %s", ZSTD_getErrorName (zr));
          const gboolean progressed = (in.pos != self->pos) || (outb.pos != done);
          self->pos = in.pos;
          done = outb.pos;
          if (zr == 0)
            self->finished = TRUE;
          else if (!progressed)
            return glnx_throw (error, "zstd stream is truncated");
        }
      break;
#endif
    default:
      g_assert_not_reached ();
    }

  *out_bytes_read = done;
  return TRUE;
}
