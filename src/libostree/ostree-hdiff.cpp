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

#include <errno.h>
#include <exception>
#include <string.h>
#include <unistd.h>
#include <vector>

#include "libHDiffPatch/HDiff/diff.h"
#include "libHDiffPatch/HDiff/private_diff/limit_mem_diff/stream_serialize.h"
#include "libHDiffPatch/HPatch/patch.h"

#include "ostree-hdiff.h"

/* Size of the cover cache handed to the patcher; the generator side keeps
 * HDiffPatch's default so memory use when applying is small and fixed.
 */
#define OT_HDIFF_STEP_MEM kDefaultPatchStepMemSize
/* Extra I/O cache given to the patcher on top of the step memory. */
#define OT_HDIFF_PATCH_IO_CACHE (1024 * 1024)

namespace
{

hpatch_BOOL
input_read (const hpatch_TStreamInput *stream, hpatch_StreamPos_t pos, unsigned char *out,
            unsigned char *out_end)
{
  const OtHdiffInput *in = static_cast<const OtHdiffInput *> (stream->streamImport);
  if (out == out_end)
    return hpatch_TRUE;
  if (pos > in->size || (hpatch_StreamPos_t)(out_end - out) > in->size - pos)
    return hpatch_FALSE;
  return in->read (in->user_data, pos, out, (gsize)(out_end - out)) ? hpatch_TRUE : hpatch_FALSE;
}

void
input_init (hpatch_TStreamInput *stream, const OtHdiffInput *in)
{
  memset (stream, 0, sizeof (*stream));
  stream->streamImport = const_cast<OtHdiffInput *> (in);
  stream->streamSize = in->size;
  stream->read = input_read;
}

struct FdOutput
{
  int fd;
  guint64 size;
  int saved_errno;
};

/* The stream differ seeks back to fill in its header, hence pwrite(). */
hpatch_BOOL
fd_write (const hpatch_TStreamOutput *stream, hpatch_StreamPos_t pos, const unsigned char *data,
          const unsigned char *data_end)
{
  FdOutput *out = static_cast<FdOutput *> (stream->streamImport);
  while (data < data_end)
    {
      ssize_t n = pwrite (out->fd, data, (size_t)(data_end - data), (off_t)pos);
      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          out->saved_errno = errno;
          return hpatch_FALSE;
        }
      data += n;
      pos += (hpatch_StreamPos_t)n;
    }
  if (pos > out->size)
    out->size = pos;
  return hpatch_TRUE;
}

struct SeqOutput
{
  OtHdiffWrite write;
  gpointer user_data;
  guint64 pos;
};

hpatch_BOOL
seq_write (const hpatch_TStreamOutput *stream, hpatch_StreamPos_t pos, const unsigned char *data,
           const unsigned char *data_end)
{
  SeqOutput *out = static_cast<SeqOutput *> (stream->streamImport);
  if (pos != out->pos)
    return hpatch_FALSE;
  if (data == data_end)
    return hpatch_TRUE;
  if (!out->write (out->user_data, data, (gsize)(data_end - data)))
    return hpatch_FALSE;
  out->pos += (guint64)(data_end - data);
  return hpatch_TRUE;
}

struct PatchListener
{
  sspatch_listener_t base;
  guint64 expected_new_size;
  guint64 old_size;
  guint8 *cache;
  const char *failure;
};

hpatch_BOOL
on_diff_info (sspatch_listener_t *listener, const hpatch_singleCompressedDiffInfo *info,
              hpatch_TDecompress **out_decompress, unsigned char **out_cache,
              unsigned char **out_cache_end)
{
  PatchListener *self = static_cast<PatchListener *> (listener->import);
  if (info->compressType[0] != '\0')
    {
      self->failure = "diff uses an embedded compressor";
      return hpatch_FALSE;
    }
  if (info->newDataSize != self->expected_new_size)
    {
      self->failure = "diff target size does not match";
      return hpatch_FALSE;
    }
  if (info->oldDataSize != self->old_size)
    {
      self->failure = "diff source size does not match";
      return hpatch_FALSE;
    }
  /* Bound what an untrusted diff can make us allocate */
  if (info->stepMemSize > 64 * 1024 * 1024)
    {
      self->failure = "diff requests too much patch memory";
      return hpatch_FALSE;
    }
  const gsize cache_size = (gsize)info->stepMemSize + OT_HDIFF_PATCH_IO_CACHE;
  self->cache = static_cast<guint8 *> (g_try_malloc (cache_size));
  if (self->cache == NULL)
    {
      self->failure = "out of memory";
      return hpatch_FALSE;
    }
  *out_decompress = NULL;
  *out_cache = self->cache;
  *out_cache_end = self->cache + cache_size;
  return hpatch_TRUE;
}

void
listener_init (PatchListener *self, guint64 expected_new_size, guint64 old_size)
{
  memset (self, 0, sizeof (*self));
  self->base.import = self;
  self->base.onDiffInfo = on_diff_info;
  self->expected_new_size = expected_new_size;
  self->old_size = old_size;
}

gboolean
throw_patch_failed (PatchListener *listener, GError **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "hdiff patch failed%s%s",
               listener->failure ? ": " : "", listener->failure ? listener->failure : "");
  return FALSE;
}

} /* namespace */

gboolean
_ostree_hdiff_create_block (const OtHdiffInput *old_in, const OtHdiffInput *new_in,
                            const OtHdiffMemOptions *options, GBytes **out_diff, GError **error)
{
  const OtHdiffMemOptions defaults = OT_HDIFF_MEM_OPTIONS_DEFAULT;
  if (options == NULL)
    options = &defaults;

  hpatch_TStreamInput old_stream, new_stream;
  input_init (&old_stream, old_in);
  input_init (&new_stream, new_in);

  try
    {
      std::vector<unsigned char> diff;
      const size_t threads = MAX (options->threads, 1);
      hdiff_private::TVectorAsStreamOutput out_stream (diff);
      /* Our inputs are not safe for concurrent use; HDiffPatch then locks */
      const hdiff_TMTSets_s mtsets = { threads, threads, false, false };
      const size_t block = MAX (options->fast_match_block, kMatchBlockSize_min);
      create_single_compressed_diff_block (&new_stream, &old_stream, &out_stream, NULL,
                                           OT_HDIFF_STEP_MEM, block, options->match_score,
                                           options->big_cache, &mtsets);
      *out_diff = g_bytes_new (diff.data (), diff.size ());
    }
  catch (const std::exception &e)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "hdiff generation failed: %s", e.what ());
      return FALSE;
    }
  return TRUE;
}

gboolean
_ostree_hdiff_create_stream (const OtHdiffInput *old_in, const OtHdiffInput *new_in, int out_fd,
                             gsize match_block, guint threads, guint64 *out_diff_size,
                             GError **error)
{
  hpatch_TStreamInput old_stream, new_stream;
  input_init (&old_stream, old_in);
  input_init (&new_stream, new_in);

  FdOutput out = { out_fd, 0, 0 };
  hpatch_TStreamOutput out_stream;
  memset (&out_stream, 0, sizeof (out_stream));
  out_stream.streamImport = &out;
  out_stream.streamSize = hpatch_kNullStreamPos;
  out_stream.write = fd_write;

  /* Our inputs serialize access themselves, so tell HDiffPatch to lock */
  const hdiff_TMTSets_s mtsets = { MAX (threads, 1), MAX (threads, 1), false, false };

  try
    {
      create_single_compressed_diff_stream (&new_stream, &old_stream, &out_stream, NULL,
                                            OT_HDIFF_STEP_MEM,
                                            MAX (match_block, kMatchBlockSize_min), &mtsets);
    }
  catch (const std::exception &e)
    {
      if (out.saved_errno != 0)
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (out.saved_errno),
                     "Writing hdiff output: %s", g_strerror (out.saved_errno));
      else
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "hdiff generation failed: %s",
                     e.what ());
      return FALSE;
    }

  if (out_diff_size)
    *out_diff_size = out.size;
  return TRUE;
}

gboolean
_ostree_hdiff_patch_stream (const OtHdiffInput *old_in, const OtHdiffInput *diff_in,
                            guint64 expected_new_size, OtHdiffWrite write, gpointer write_data,
                            GError **error)
{
  hpatch_TStreamInput old_stream, diff_stream;
  input_init (&old_stream, old_in);
  input_init (&diff_stream, diff_in);

  SeqOutput out = { write, write_data, 0 };
  hpatch_TStreamOutput out_stream;
  memset (&out_stream, 0, sizeof (out_stream));
  out_stream.streamImport = &out;
  out_stream.streamSize = expected_new_size;
  out_stream.write = seq_write;

  PatchListener listener;
  listener_init (&listener, expected_new_size, old_in->size);
  hpatch_BOOL ok
      = patch_single_stream (&listener.base, &out_stream, &old_stream, &diff_stream, 0, NULL, 1);
  g_free (listener.cache);
  if (!ok)
    return throw_patch_failed (&listener, error);
  if (out.pos != expected_new_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "hdiff patch produced %" G_GUINT64_FORMAT " bytes, expected %" G_GUINT64_FORMAT,
                   out.pos, expected_new_size);
      return FALSE;
    }
  return TRUE;
}
