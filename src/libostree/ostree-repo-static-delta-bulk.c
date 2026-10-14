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

/* "Bulk" static deltas: rather than computing a delta per file, objects are
 * concatenated into streams and one HDiffPatch diff is made between whole
 * groups of them.  See OSTREE_STATIC_DELTA_PART_BULK for the part format.
 *
 * A delta has up to two such parts:
 *
 *  - "modified": new metadata objects, plus new files whose basename matches
 *    a file that is gone from the target commit, diffed against the gone
 *    metadata and those gone files with the precise in-memory differ.  This
 *    is what an ordinary update consists of.
 *  - "new": all other new files, diffed against remaining old files with a
 *    matching basename using the fast block-matching differ.  Mostly this is
 *    content with nothing to diff against, so its cost is compression.
 */

#include "config.h"

#include <string.h>
#include <sys/stat.h>

#include "ostree-core-private.h"
#include "ostree-delta-compress.h"
#include "ostree-hdiff.h"
#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "otutil.h"

#define OBJ_NAME_LEN OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN
#define IO_BUF_SIZE (1024 * 1024)
#define FD_CACHE_SIZE 32

/* Above this much old + new data, the "modified" group is handled by the
 * block-matching differ too: the precise differ holds both in memory.
 */
#define MAX_MEM_DIFF_BYTES (G_GUINT64_CONSTANT (4) << 30)
/* Diffs up to this size are always compressed as hard as their codec goes;
 * compression time is negligible there, whatever the effort.
 */
#define SMALL_DIFF_BYTES (4 * 1024 * 1024)
/* The header is held in memory when applying; bound it. */
#define MAX_HEADER_SIZE (256 * 1024 * 1024)
/* Symlink targets and metadata are buffered when applying; bound them. */
#define MAX_SYMLINK_TARGET_SIZE (64 * 1024)

/* ------------------------------------------------------------------------ */
/* A read-only stream made by concatenating objects                          */
/* ------------------------------------------------------------------------ */

typedef struct
{
  guint64 offset;
  guint64 size;
  GBytes *mem;                                 /* in-memory segment, or... */
  char checksum[OSTREE_SHA256_STRING_LEN + 1]; /* ...a bare content object, or... */
  int ext_fd;                                  /* ...a range of a file we don't own */
  guint64 ext_offset;
} ConcatSegment;

typedef struct
{
  int fd;
  guint seg;
  guint64 stamp;
} ConcatCachedFd;

typedef struct
{
  OstreeRepo *repo;
  GArray *segs;
  guint64 size;
  GMutex lock;
  guint hint;
  guint64 clock;
  ConcatCachedFd cache[FD_CACHE_SIZE];
  GError *error; /* first read error */
} Concat;

static void
concat_segment_clear (gpointer data)
{
  ConcatSegment *seg = data;
  g_clear_pointer (&seg->mem, g_bytes_unref);
}

static Concat *
concat_new (OstreeRepo *repo)
{
  Concat *self = g_new0 (Concat, 1);
  self->repo = repo;
  self->segs = g_array_new (FALSE, TRUE, sizeof (ConcatSegment));
  g_array_set_clear_func (self->segs, concat_segment_clear);
  g_mutex_init (&self->lock);
  for (guint i = 0; i < FD_CACHE_SIZE; i++)
    self->cache[i].fd = -1;
  return self;
}

static void
concat_free (Concat *self)
{
  if (self == NULL)
    return;
  for (guint i = 0; i < FD_CACHE_SIZE; i++)
    glnx_close_fd (&self->cache[i].fd);
  g_array_unref (self->segs);
  g_mutex_clear (&self->lock);
  g_clear_error (&self->error);
  g_free (self);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Concat, concat_free)

static ConcatSegment *
concat_append (Concat *self, guint64 size)
{
  ConcatSegment seg = {
    0,
  };
  seg.offset = self->size;
  seg.size = size;
  seg.ext_fd = -1;
  g_array_append_val (self->segs, seg);
  self->size += size;
  return &g_array_index (self->segs, ConcatSegment, self->segs->len - 1);
}

static void
concat_append_bytes (Concat *self, GBytes *bytes)
{
  ConcatSegment *seg = concat_append (self, g_bytes_get_size (bytes));
  seg->mem = g_bytes_ref (bytes);
}

static void
concat_append_object (Concat *self, const char *checksum, guint64 size)
{
  ConcatSegment *seg = concat_append (self, size);
  memcpy (seg->checksum, checksum, OSTREE_SHA256_STRING_LEN + 1);
}

static void
concat_append_fd_range (Concat *self, int fd, guint64 offset, guint64 size)
{
  ConcatSegment *seg = concat_append (self, size);
  seg->ext_fd = fd;
  seg->ext_offset = offset;
}

/* Must be called with the lock held */
static int
concat_get_fd (Concat *self, guint segidx, GError **error)
{
  ConcatSegment *seg = &g_array_index (self->segs, ConcatSegment, segidx);
  if (seg->ext_fd != -1)
    return seg->ext_fd;

  /* Reuse a cached descriptor, or else replace an unused or the least
   * recently used slot.
   */
  guint victim = 0;
  for (guint i = 0; i < FD_CACHE_SIZE; i++)
    {
      if (self->cache[i].fd == -1)
        {
          if (self->cache[victim].fd != -1)
            victim = i;
          continue;
        }
      if (self->cache[i].seg == segidx)
        {
          self->cache[i].stamp = ++self->clock;
          return self->cache[i].fd;
        }
      if (self->cache[victim].fd != -1 && self->cache[i].stamp < self->cache[victim].stamp)
        victim = i;
    }

  glnx_autofd int fd = -1;
  struct stat stbuf;
  if (!_ostree_repo_load_file_bare (self->repo, seg->checksum, &fd, &stbuf, NULL, NULL, NULL,
                                    error))
    return -1;
  if (fd == -1 || !S_ISREG (stbuf.st_mode))
    return glnx_throw (error, "Object %s is not a regular file", seg->checksum), -1;
  if ((guint64)stbuf.st_size != seg->size)
    return glnx_throw (error, "Object %s has unexpected size", seg->checksum), -1;

  glnx_close_fd (&self->cache[victim].fd);
  self->cache[victim].fd = glnx_steal_fd (&fd);
  self->cache[victim].seg = segidx;
  self->cache[victim].stamp = ++self->clock;
  return self->cache[victim].fd;
}

static gboolean
concat_read_locked (Concat *self, guint64 pos, guint8 *buf, gsize len, GError **error)
{
  if (pos > self->size || len > self->size - pos)
    return glnx_throw (error, "Read beyond the end of object stream");

  /* Find the segment containing pos, starting from where we last were */
  guint idx = self->hint;
  if (idx >= self->segs->len || g_array_index (self->segs, ConcatSegment, idx).offset > pos)
    idx = 0;
  {
    guint lo = idx, hi = self->segs->len;
    while (hi - lo > 1)
      {
        guint mid = lo + (hi - lo) / 2;
        if (g_array_index (self->segs, ConcatSegment, mid).offset <= pos)
          lo = mid;
        else
          hi = mid;
      }
    idx = lo;
  }

  while (len > 0)
    {
      g_assert (idx < self->segs->len);
      ConcatSegment *seg = &g_array_index (self->segs, ConcatSegment, idx);
      const guint64 segpos = pos - seg->offset;
      if (segpos >= seg->size)
        {
          idx++;
          continue;
        }
      const gsize n = MIN ((guint64)len, seg->size - segpos);
      if (seg->mem)
        memcpy (buf, (const guint8 *)g_bytes_get_data (seg->mem, NULL) + segpos, n);
      else
        {
          int fd = concat_get_fd (self, idx, error);
          if (fd < 0)
            return FALSE;
          gsize done = 0;
          while (done < n)
            {
              ssize_t r = TEMP_FAILURE_RETRY (
                  pread (fd, buf + done, n - done, seg->ext_offset + segpos + done));
              if (r < 0)
                return glnx_throw_errno_prefix (error, "pread");
              if (r == 0)
                return glnx_throw (error, "Unexpected end of object data");
              done += r;
            }
        }
      buf += n;
      pos += n;
      len -= n;
    }
  self->hint = idx;
  return TRUE;
}

static gboolean
concat_read (gpointer user_data, guint64 pos, guint8 *buf, gsize len)
{
  Concat *self = user_data;
  g_mutex_lock (&self->lock);
  g_autoptr (GError) local_error = NULL;
  gboolean ret = concat_read_locked (self, pos, buf, len, &local_error);
  if (!ret && self->error == NULL)
    self->error = g_steal_pointer (&local_error);
  g_mutex_unlock (&self->lock);
  return ret;
}

static OtHdiffInput
concat_as_input (Concat *self)
{
  OtHdiffInput in = { self->size, concat_read, self };
  return in;
}

/* Turn a failure of something reading from @self into the underlying error */
static gboolean
concat_propagate_error (Concat *self, GError **error)
{
  if (self != NULL && self->error != NULL)
    {
      if (error != NULL && *error != NULL)
        g_clear_error (error);
      g_propagate_error (error, g_steal_pointer (&self->error));
    }
  return FALSE;
}

typedef struct
{
  Concat *concat;
  guint64 pos;
} ConcatCursor;

static gssize
concat_cursor_read (gpointer user_data, guint8 *buf, gsize len, GError **error)
{
  ConcatCursor *cursor = user_data;
  const gsize n = MIN ((guint64)len, cursor->concat->size - cursor->pos);
  if (n == 0)
    return 0;
  if (!concat_read (cursor->concat, cursor->pos, buf, n))
    {
      glnx_throw (error, "Failed to read object stream");
      concat_propagate_error (cursor->concat, error);
      return -1;
    }
  cursor->pos += n;
  return n;
}

/* ------------------------------------------------------------------------ */
/* Generation                                                                */
/* ------------------------------------------------------------------------ */

typedef struct
{
  guint8 compression;
  int level;
  /* log2 of the zstd window (which from 25 also turns on long distance
   * matching) or of the xz dictionary; 0 for the default of the level.
   */
  guint window_log;
  /* xz only: one block, compressed on a single thread */
  gboolean single_block;
  /* Block size for the block-matching differ */
  gsize stream_block;
} EffortConfig;

/* From "as fast as possible" (tight development loops) to "as small as
 * possible" (updates sent over expensive links).  Each step was chosen from
 * measurements on operating system images to be both smaller and slower than
 * the one before it; settings that another one beat on both counts (xz at
 * low presets, zstd 13-15, zstd 19 and above) are absent.
 *
 * Up to 7 the data is zstd compressed.  Decompressing zstd costs the same at
 * every level, so those efforts differ only in the time to generate.  8 and
 * 9 use xz, which is smaller still but takes two to three times as long to
 * apply.  The window is also the memory needed to decompress: at most
 * 32 MiB up to effort 3, 128 MiB above.
 *
 * The block-matching differ's block size only matters when there is a lot
 * of new content; large blocks are about 20% faster and 1% larger.
 */
static const EffortConfig effort_table[OSTREE_STATIC_DELTA_BULK_EFFORT_MAX + 1] = {
  /* 0 */ { OSTREE_STATIC_DELTA_COMPRESSION_ZSTD, 1, 0, FALSE, 1024 },
  /* 1 */ { OSTREE_STATIC_DELTA_COMPRESSION_ZSTD, 3, 0, FALSE, 256 },
  /* 2 */ { OSTREE_STATIC_DELTA_COMPRESSION_ZSTD, 6, 0, FALSE, 64 },
  /* 3 */ { OSTREE_STATIC_DELTA_COMPRESSION_ZSTD, 8, 25, FALSE, 64 },
  /* 4 */ { OSTREE_STATIC_DELTA_COMPRESSION_ZSTD, 10, 27, FALSE, 64 },
  /* 5 */ { OSTREE_STATIC_DELTA_COMPRESSION_ZSTD, 12, 27, FALSE, 64 },
  /* 6 */ { OSTREE_STATIC_DELTA_COMPRESSION_ZSTD, 16, 27, FALSE, 64 },
  /* 7 */ { OSTREE_STATIC_DELTA_COMPRESSION_ZSTD, 18, 27, FALSE, 64 },
  /* 8 */ { OSTREE_STATIC_DELTA_COMPRESSION_XZ, 6, 25, FALSE, 64 },
  /* 9 */ { OSTREE_STATIC_DELTA_COMPRESSION_XZ, 9, 27, TRUE, 64 },
};

typedef struct
{
  OstreeObjectType objtype;
  char checksum[OSTREE_SHA256_STRING_LEN + 1];
  const char *path; /* owned by the walk tables */
  guint64 size;     /* in the stream */
  GBytes *mem;      /* metadata bytes or symlink target */
  guint32 mode_index;
  guint32 xattr_index;
} StreamObject;

static void
stream_object_clear (gpointer data)
{
  StreamObject *obj = data;
  g_clear_pointer (&obj->mem, g_bytes_unref);
}

static GArray *
stream_object_array_new (void)
{
  GArray *ret = g_array_new (FALSE, TRUE, sizeof (StreamObject));
  g_array_set_clear_func (ret, stream_object_clear);
  return ret;
}

/* Metadata first, then content; each ordered by path so that related data
 * ends up close together in both streams.
 */
static int
stream_object_compare (gconstpointer ap, gconstpointer bp)
{
  const StreamObject *a = ap;
  const StreamObject *b = bp;
  const gboolean a_meta = OSTREE_OBJECT_TYPE_IS_META (a->objtype);
  const gboolean b_meta = OSTREE_OBJECT_TYPE_IS_META (b->objtype);
  if (a_meta != b_meta)
    return a_meta ? -1 : 1;
  int c = strcmp (a->path, b->path);
  if (c != 0)
    return c;
  if (a->objtype != b->objtype)
    return a->objtype < b->objtype ? -1 : 1;
  return strcmp (a->checksum, b->checksum);
}

typedef struct
{
  GHashTable *files;    /* checksum -> first path */
  GHashTable *dirtrees; /* checksum -> first path */
  GHashTable *dirmetas; /* checksum -> first path */
} CommitContents;

static void
commit_contents_clear (CommitContents *self)
{
  g_clear_pointer (&self->files, g_hash_table_unref);
  g_clear_pointer (&self->dirtrees, g_hash_table_unref);
  g_clear_pointer (&self->dirmetas, g_hash_table_unref);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (CommitContents, commit_contents_clear)

static void
commit_contents_init (CommitContents *self)
{
  self->files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->dirtrees = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->dirmetas = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

static gboolean
walk_dirtree (OstreeRepo *repo, const char *tree_checksum, GString *path, CommitContents *out,
              GCancellable *cancellable, GError **error)
{
  /* Identical subtrees have identical contents */
  if (g_hash_table_contains (out->dirtrees, tree_checksum))
    return TRUE;
  g_hash_table_insert (out->dirtrees, g_strdup (tree_checksum), g_strdup (path->str));

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_autoptr (GVariant) tree = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_TREE, tree_checksum, &tree, error))
    return FALSE;

  g_autoptr (GVariant) files = g_variant_get_child_value (tree, 0);
  g_autoptr (GVariant) dirs = g_variant_get_child_value (tree, 1);
  const gsize base_len = path->len;

  const gsize n_files = g_variant_n_children (files);
  for (gsize i = 0; i < n_files; i++)
    {
      const char *name;
      g_autoptr (GVariant) csum_v = NULL;
      g_variant_get_child (files, i, "(&s@ay)", &name, &csum_v);
      const guchar *csum = ostree_checksum_bytes_peek_validate (csum_v, error);
      if (!csum)
        return FALSE;
      char checksum[OSTREE_SHA256_STRING_LEN + 1];
      ostree_checksum_inplace_from_bytes (csum, checksum);
      if (!g_hash_table_contains (out->files, checksum))
        g_hash_table_insert (out->files, g_strdup (checksum),
                             g_strconcat (path->str, "/", name, NULL));
    }

  const gsize n_dirs = g_variant_n_children (dirs);
  for (gsize i = 0; i < n_dirs; i++)
    {
      const char *name;
      g_autoptr (GVariant) tree_csum_v = NULL;
      g_autoptr (GVariant) meta_csum_v = NULL;
      g_variant_get_child (dirs, i, "(&s@ay@ay)", &name, &tree_csum_v, &meta_csum_v);
      const guchar *tree_csum = ostree_checksum_bytes_peek_validate (tree_csum_v, error);
      if (!tree_csum)
        return FALSE;
      const guchar *meta_csum = ostree_checksum_bytes_peek_validate (meta_csum_v, error);
      if (!meta_csum)
        return FALSE;
      char checksum[OSTREE_SHA256_STRING_LEN + 1];

      g_string_append_c (path, '/');
      g_string_append (path, name);

      ostree_checksum_inplace_from_bytes (meta_csum, checksum);
      if (!g_hash_table_contains (out->dirmetas, checksum))
        g_hash_table_insert (out->dirmetas, g_strdup (checksum), g_strdup (path->str));

      ostree_checksum_inplace_from_bytes (tree_csum, checksum);
      if (!walk_dirtree (repo, checksum, path, out, cancellable, error))
        return FALSE;

      g_string_truncate (path, base_len);
    }

  return TRUE;
}

static gboolean
walk_commit (OstreeRepo *repo, const char *commit_checksum, CommitContents *out,
             GCancellable *cancellable, GError **error)
{
  g_autoptr (GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit, error))
    return FALSE;

  g_autoptr (GVariant) tree_csum_v = NULL;
  g_autoptr (GVariant) meta_csum_v = NULL;
  g_variant_get_child (commit, 6, "@ay", &tree_csum_v);
  g_variant_get_child (commit, 7, "@ay", &meta_csum_v);
  const guchar *tree_csum = ostree_checksum_bytes_peek_validate (tree_csum_v, error);
  if (!tree_csum)
    return FALSE;
  const guchar *meta_csum = ostree_checksum_bytes_peek_validate (meta_csum_v, error);
  if (!meta_csum)
    return FALSE;

  char checksum[OSTREE_SHA256_STRING_LEN + 1];
  ostree_checksum_inplace_from_bytes (meta_csum, checksum);
  g_hash_table_insert (out->dirmetas, g_strdup (checksum), g_strdup (""));

  ostree_checksum_inplace_from_bytes (tree_csum, checksum);
  g_autoptr (GString) path = g_string_new ("");
  return walk_dirtree (repo, checksum, path, out, cancellable, error);
}

static const char *
path_basename (const char *path)
{
  const char *slash = strrchr (path, '/');
  return slash ? slash + 1 : path;
}

/* The name files are grouped by: the basename with every run of digits collapsed, so that
 * libfoo.so.1.2.3 and libfoo.so.1.3.0 count as the same file.  Owned by @chunk.
 */
static const char *
route_name (GStringChunk *chunk, const char *path)
{
  g_autoptr (GString) s = g_string_new (NULL);
  for (const char *p = path_basename (path); *p; p++)
    {
      if (!g_ascii_isdigit (*p))
        g_string_append_c (s, *p);
      else if (s->len == 0 || s->str[s->len - 1] != '0')
        g_string_append_c (s, '0');
    }
  return g_string_chunk_insert_const (chunk, s->str);
}

typedef struct
{
  GPtrArray *values; /* GVariant */
  GHashTable *index; /* GBytes -> index */
} VariantTable;

static void
variant_table_init (VariantTable *self)
{
  self->values = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  self->index
      = g_hash_table_new_full (g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref, NULL);
}

static void
variant_table_clear (VariantTable *self)
{
  g_clear_pointer (&self->values, g_ptr_array_unref);
  g_clear_pointer (&self->index, g_hash_table_unref);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (VariantTable, variant_table_clear)

static guint32
variant_table_intern (VariantTable *self, GVariant *value)
{
  g_autoptr (GVariant) normal = g_variant_get_normal_form (value);
  g_autoptr (GBytes) key = g_variant_get_data_as_bytes (normal);
  gpointer existing;
  if (g_hash_table_lookup_extended (self->index, key, NULL, &existing))
    return GPOINTER_TO_UINT (existing);
  const guint32 idx = self->values->len;
  g_ptr_array_add (self->values, g_steal_pointer (&normal));
  g_hash_table_insert (self->index, g_steal_pointer (&key), GUINT_TO_POINTER (idx));
  return idx;
}

typedef struct
{
  int fd;
  OtChecksum *checksum;
  guint64 size;
} PartWriter;

static gboolean
part_writer_write (gpointer user_data, const guint8 *buf, gsize len, GError **error)
{
  PartWriter *self = user_data;
  if (glnx_loop_write (self->fd, buf, len) < 0)
    return glnx_throw_errno_prefix (error, "Writing delta part");
  ot_checksum_update (self->checksum, buf, len);
  self->size += len;
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Pipeline stages                                                           */
/* ------------------------------------------------------------------------ */

/* Compressing a large part is a pipeline: read the objects, compress, then
 * write and checksum the result.  The compressors only parallelise the
 * middle stage, and at low efforts the two ends cost more than it does.  The
 * helpers below move each end onto a thread of its own.  The bytes produced
 * are the same.
 */
#define PIPE_DEPTH 8
#define SOURCE_CHUNK_SIZE (4 * 1024 * 1024)
#define SINK_CHUNK_SIZE (1024 * 1024)

/* A bounded queue of buffers handed from one thread to another */
typedef struct
{
  GMutex lock;
  GCond cond;
  GQueue queue;     /* GBytes */
  gboolean closed;  /* the producer has finished */
  gboolean aborted; /* either side has given up */
} Pipe;

static void
pipe_init (Pipe *self)
{
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
  g_queue_init (&self->queue);
  self->closed = self->aborted = FALSE;
}

static void
pipe_clear (Pipe *self)
{
  g_queue_clear_full (&self->queue, (GDestroyNotify)g_bytes_unref);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
}

/* Takes ownership of @bytes; FALSE if the other side has given up */
static gboolean
pipe_push (Pipe *self, GBytes *bytes)
{
  g_mutex_lock (&self->lock);
  while (!self->aborted && g_queue_get_length (&self->queue) >= PIPE_DEPTH)
    g_cond_wait (&self->cond, &self->lock);
  const gboolean ok = !self->aborted;
  if (ok)
    g_queue_push_tail (&self->queue, bytes);
  else
    g_bytes_unref (bytes);
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  return ok;
}

/* The next buffer, or NULL once the producer has finished or either side gave up */
static GBytes *
pipe_pop (Pipe *self)
{
  g_mutex_lock (&self->lock);
  while (!self->aborted && !self->closed && g_queue_is_empty (&self->queue))
    g_cond_wait (&self->cond, &self->lock);
  GBytes *ret = self->aborted ? NULL : g_queue_pop_head (&self->queue);
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  return ret;
}

static void
pipe_finish (Pipe *self, gboolean abort)
{
  g_mutex_lock (&self->lock);
  if (abort)
    self->aborted = TRUE;
  else
    self->closed = TRUE;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
}

/* Runs an OtDeltaReadFunc ahead of its consumer, on its own thread */
typedef struct
{
  Pipe pipe;
  GThread *thread;
  OtDeltaReadFunc read;
  gpointer read_data;
  GBytes *current;
  gsize pos;
  GError *error; /* set by the thread before it aborts the pipe */
} AsyncSource;

static gpointer
async_source_thread (gpointer data)
{
  AsyncSource *self = data;
  while (TRUE)
    {
      guint8 *buf = g_malloc (SOURCE_CHUNK_SIZE);
      gssize n = self->read (self->read_data, buf, SOURCE_CHUNK_SIZE, &self->error);
      if (n <= 0)
        {
          g_free (buf);
          pipe_finish (&self->pipe, n < 0);
          break;
        }
      if (!pipe_push (&self->pipe, g_bytes_new_take (buf, n)))
        break;
    }
  return NULL;
}

static void
async_source_start (AsyncSource *self, OtDeltaReadFunc read, gpointer read_data)
{
  memset (self, 0, sizeof (*self));
  pipe_init (&self->pipe);
  self->read = read;
  self->read_data = read_data;
  self->thread = g_thread_new ("ostree-delta-read", async_source_thread, self);
}

static gssize
async_source_read (gpointer user_data, guint8 *buf, gsize len, GError **error)
{
  AsyncSource *self = user_data;
  if (self->current == NULL)
    {
      self->current = pipe_pop (&self->pipe);
      self->pos = 0;
      if (self->current == NULL)
        {
          /* End of input, or the reader failed: its error is final once the pipe aborted */
          g_mutex_lock (&self->pipe.lock);
          const gboolean failed = self->pipe.aborted;
          g_mutex_unlock (&self->pipe.lock);
          if (!failed)
            return 0;
          if (self->error != NULL)
            g_propagate_error (error, g_error_copy (self->error));
          else
            glnx_throw (error, "Reading delta content failed");
          return -1;
        }
    }
  gsize size;
  const guint8 *data = g_bytes_get_data (self->current, &size);
  const gsize n = MIN (len, size - self->pos);
  memcpy (buf, data + self->pos, n);
  self->pos += n;
  if (self->pos == size)
    g_clear_pointer (&self->current, g_bytes_unref);
  return n;
}

/* Must be called exactly once, whether or not everything was read */
static void
async_source_finish (AsyncSource *self)
{
  pipe_finish (&self->pipe, TRUE);
  g_thread_join (self->thread);
  g_clear_pointer (&self->current, g_bytes_unref);
  g_clear_error (&self->error);
  pipe_clear (&self->pipe);
}

/* Runs an OtDeltaWriteFunc behind its producer, on its own thread */
typedef struct
{
  Pipe pipe;
  GThread *thread;
  OtDeltaWriteFunc write;
  gpointer write_data;
  GByteArray *pending;
  GError *error; /* set by the thread before it aborts the pipe */
} AsyncSink;

static gpointer
async_sink_thread (gpointer data)
{
  AsyncSink *self = data;
  GBytes *bytes;
  while ((bytes = pipe_pop (&self->pipe)) != NULL)
    {
      gsize size;
      const guint8 *buf = g_bytes_get_data (bytes, &size);
      const gboolean ok = self->write (self->write_data, buf, size, &self->error);
      g_bytes_unref (bytes);
      if (!ok)
        {
          pipe_finish (&self->pipe, TRUE);
          break;
        }
    }
  return NULL;
}

static void
async_sink_start (AsyncSink *self, OtDeltaWriteFunc write, gpointer write_data)
{
  memset (self, 0, sizeof (*self));
  pipe_init (&self->pipe);
  self->write = write;
  self->write_data = write_data;
  self->pending = g_byte_array_sized_new (SINK_CHUNK_SIZE);
  self->thread = g_thread_new ("ostree-delta-write", async_sink_thread, self);
}

static gboolean
async_sink_flush (AsyncSink *self, GError **error)
{
  if (self->pending->len == 0)
    return TRUE;
  GBytes *bytes = g_byte_array_free_to_bytes (self->pending);
  self->pending = g_byte_array_sized_new (SINK_CHUNK_SIZE);
  if (!pipe_push (&self->pipe, bytes))
    return glnx_throw (error,
                       "Writing delta part failed"); /* replaced by the real error at finish */
  return TRUE;
}

static gboolean
async_sink_write (gpointer user_data, const guint8 *buf, gsize len, GError **error)
{
  AsyncSink *self = user_data;
  g_byte_array_append (self->pending, buf, len);
  if (self->pending->len >= SINK_CHUNK_SIZE)
    return async_sink_flush (self, error);
  return TRUE;
}

/* Must be called exactly once.  @ok is whether the producer succeeded; returns
 * whether everything was written, preferring the writer's own error.
 */
static gboolean
async_sink_finish (AsyncSink *self, gboolean ok, GError **error)
{
  g_autoptr (GError) flush_error = NULL;
  if (ok && !async_sink_flush (self, &flush_error))
    ok = FALSE;
  pipe_finish (&self->pipe, !ok);
  g_thread_join (self->thread);
  g_byte_array_unref (self->pending);
  pipe_clear (&self->pipe);
  if (self->error != NULL)
    {
      if (error != NULL)
        g_clear_error (error);
      g_propagate_error (error, g_steal_pointer (&self->error));
      return FALSE;
    }
  if (flush_error != NULL && error != NULL && *error == NULL)
    g_propagate_error (error, g_steal_pointer (&flush_error));
  return ok;
}

static gboolean
bytearray_writer_write (gpointer user_data, const guint8 *buf, gsize len, GError **error)
{
  g_byte_array_append (user_data, buf, len);
  return TRUE;
}

typedef struct
{
  int fd;
} FdReader;

static gssize
fd_reader_read (gpointer user_data, guint8 *buf, gsize len, GError **error)
{
  FdReader *self = user_data;
  ssize_t r = TEMP_FAILURE_RETRY (read (self->fd, buf, len));
  if (r < 0)
    return glnx_throw_errno_prefix (error, "read"), -1;
  return r;
}

typedef struct
{
  const guint8 *data;
  gsize len;
  gsize pos;
} MemReader;

static gssize
mem_reader_read (gpointer user_data, guint8 *buf, gsize len, GError **error)
{
  MemReader *self = user_data;
  const gsize n = MIN (len, self->len - self->pos);
  memcpy (buf, self->data + self->pos, n);
  self->pos += n;
  return n;
}

typedef struct
{
  OstreeRepo *repo;
  const OstreeStaticDeltaBulkOptions *options;
  const EffortConfig *effort;
  guint threads;
  int parts_dfd;
  GPtrArray *out_parts;
  /* Non-bare repositories cannot be read by random access; their file
   * content is unpacked here first.
   */
  GLnxTmpfile unpack_tmpf;
  guint64 unpack_size;
  gboolean bare;
} BulkGenerator;

/* Add the content of regular file @checksum to @concat */
static gboolean
generator_append_file (BulkGenerator *self, Concat *concat, const char *checksum, guint64 size,
                       GCancellable *cancellable, GError **error)
{
  if (self->bare)
    {
      concat_append_object (concat, checksum, size);
      return TRUE;
    }

  if (!self->unpack_tmpf.initialized
      && !glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &self->unpack_tmpf, error))
    return FALSE;

  g_autoptr (GInputStream) in = NULL;
  if (!ostree_repo_load_file (self->repo, checksum, &in, NULL, NULL, cancellable, error))
    return FALSE;
  if (in == NULL)
    return glnx_throw (error, "Object %s is not a regular file", checksum);

  if (lseek (self->unpack_tmpf.fd, self->unpack_size, SEEK_SET) < 0)
    return glnx_throw_errno_prefix (error, "lseek");
  g_autofree guint8 *buf = g_malloc (IO_BUF_SIZE);
  guint64 total = 0;
  while (TRUE)
    {
      gsize n;
      if (!g_input_stream_read_all (in, buf, IO_BUF_SIZE, &n, cancellable, error))
        return FALSE;
      if (n == 0)
        break;
      if (glnx_loop_write (self->unpack_tmpf.fd, buf, n) < 0)
        return glnx_throw_errno_prefix (error, "write");
      total += n;
    }
  if (total != size)
    return glnx_throw (error, "Object %s has unexpected size", checksum);

  concat_append_fd_range (concat, self->unpack_tmpf.fd, self->unpack_size, size);
  self->unpack_size += size;
  return TRUE;
}

static gboolean
generator_build_concat (BulkGenerator *self, GArray *objects, Concat **out_concat,
                        GCancellable *cancellable, GError **error)
{
  g_autoptr (Concat) concat = concat_new (self->repo);
  for (guint i = 0; i < objects->len; i++)
    {
      StreamObject *obj = &g_array_index (objects, StreamObject, i);
      if (obj->mem)
        concat_append_bytes (concat, obj->mem);
      else if (!generator_append_file (self, concat, obj->checksum, obj->size, cancellable, error))
        return FALSE;
    }
  *out_concat = g_steal_pointer (&concat);
  return TRUE;
}

static GVariant *
maybe_swap_u64 (gboolean swap, guint64 v)
{
  return g_variant_new_uint64 (swap ? GUINT64_SWAP_LE_BE (v) : v);
}

/* Emit one part producing @new_objects, optionally from @old_objects.
 * @precise selects the in-memory differ.
 */
static gboolean
generator_emit_part (BulkGenerator *self, const char *name, guint part_index, GArray *new_objects,
                     GArray *old_objects, gboolean precise, VariantTable *modes,
                     VariantTable *xattrs, GCancellable *cancellable, GError **error)
{
  OstreeStaticDeltaBulkPart *part = self->out_parts->pdata[part_index];

  const gint64 start_time = g_get_monotonic_time ();

  g_autoptr (Concat) new_concat = NULL;
  if (!generator_build_concat (self, new_objects, &new_concat, cancellable, error))
    return FALSE;
  g_autoptr (Concat) old_concat = NULL;
  if (!generator_build_concat (self, old_objects, &old_concat, cancellable, error))
    return FALSE;

  const gboolean use_diff = old_concat->size > 0 && new_concat->size > 0;
  g_autoptr (GBytes) mem_diff = NULL;
  g_auto (GLnxTmpfile) diff_tmpf = {
    0,
  };
  guint64 data_size;

  if (!use_diff)
    data_size = new_concat->size;
  else
    {
#ifdef HAVE_HDIFFPATCH
      const OtHdiffInput old_in = concat_as_input (old_concat);
      const OtHdiffInput new_in = concat_as_input (new_concat);
      gboolean ok;
      if (precise && old_concat->size + new_concat->size <= MAX_MEM_DIFF_BYTES)
        {
          OtHdiffMemOptions opts = OT_HDIFF_MEM_OPTIONS_DEFAULT;
          opts.threads = MIN (self->threads, 4);
          ok = _ostree_hdiff_create_block (&old_in, &new_in, &opts, &mem_diff, error);
          if (ok)
            data_size = g_bytes_get_size (mem_diff);
        }
      else
        {
          if (!glnx_open_tmpfile_linkable_at (self->parts_dfd, ".", O_RDWR | O_CLOEXEC, &diff_tmpf,
                                              error))
            return FALSE;
          ok = _ostree_hdiff_create_stream (&old_in, &new_in, diff_tmpf.fd,
                                            self->effort->stream_block, self->threads, &data_size,
                                            error);
        }
      if (!ok)
        {
          concat_propagate_error (old_concat, error);
          concat_propagate_error (new_concat, error);
          return FALSE;
        }
#else
      g_assert_not_reached ();
#endif
    }

  /* Pick the compressor: small diffs always get the strongest setting of
   * the effort's codec, since the time is negligible.
   */
  OtDeltaCompressOptions copts = {
    0,
  };
  copts.type = self->effort->compression;
  copts.level = self->effort->level;
  copts.window_log = self->effort->window_log;
  copts.threads = self->effort->single_block ? 1 : self->threads;
  if (data_size <= SMALL_DIFF_BYTES)
    {
      copts.threads = 1;
      if (copts.type == OSTREE_STATIC_DELTA_COMPRESSION_ZSTD)
        copts.level = MAX (copts.level, 19);
      else
        {
          copts.level = 9;
          copts.window_log = MAX (copts.window_log, 23);
        }
    }

  /* The header */
  g_autoptr (GByteArray) sources = g_byte_array_new ();
  if (use_diff)
    for (guint i = 0; i < old_objects->len; i++)
      {
        StreamObject *obj = &g_array_index (old_objects, StreamObject, i);
        guint8 name_bytes[OBJ_NAME_LEN];
        name_bytes[0] = (guint8)obj->objtype;
        ostree_checksum_inplace_to_bytes (obj->checksum, name_bytes + 1);
        g_byte_array_append (sources, name_bytes, sizeof (name_bytes));
      }

  g_auto (GVariantBuilder) modes_builder = OT_VARIANT_BUILDER_INITIALIZER;
  g_variant_builder_init (&modes_builder, G_VARIANT_TYPE ("a(uuu)"));
  for (guint i = 0; i < modes->values->len; i++)
    g_variant_builder_add_value (&modes_builder, modes->values->pdata[i]);
  g_auto (GVariantBuilder) xattrs_builder = OT_VARIANT_BUILDER_INITIALIZER;
  g_variant_builder_init (&xattrs_builder, G_VARIANT_TYPE ("aa(ayay)"));
  for (guint i = 0; i < xattrs->values->len; i++)
    g_variant_builder_add_value (&xattrs_builder, xattrs->values->pdata[i]);

  g_auto (GVariantBuilder) entries_builder = OT_VARIANT_BUILDER_INITIALIZER;
  g_variant_builder_init (&entries_builder, G_VARIANT_TYPE ("a(uut)"));
  g_autoptr (GByteArray) object_names = g_byte_array_new ();
  for (guint i = 0; i < new_objects->len; i++)
    {
      StreamObject *obj = &g_array_index (new_objects, StreamObject, i);
      g_variant_builder_add (&entries_builder, "(uut)", GUINT32_TO_LE (obj->mode_index),
                             GUINT32_TO_LE (obj->xattr_index), GUINT64_TO_LE (obj->size));
      guint8 name_bytes[OBJ_NAME_LEN];
      name_bytes[0] = (guint8)obj->objtype;
      ostree_checksum_inplace_to_bytes (obj->checksum, name_bytes + 1);
      g_byte_array_append (object_names, name_bytes, sizeof (name_bytes));
    }

  g_autoptr (GVariant) header = g_variant_ref_sink (g_variant_new (
      "(yytt@ay@a(uuu)@aa(ayay)@a(uut))", copts.type,
      (guint8)(use_diff ? OSTREE_STATIC_DELTA_BULK_KIND_HDIFF : OSTREE_STATIC_DELTA_BULK_KIND_RAW),
      GUINT64_TO_LE (new_concat->size), GUINT64_TO_LE (data_size),
      ot_gvariant_new_bytearray (sources->data, sources->len),
      g_variant_builder_end (&modes_builder), g_variant_builder_end (&xattrs_builder),
      g_variant_builder_end (&entries_builder)));

  /* Write the part */
  if (!glnx_open_tmpfile_linkable_at (self->parts_dfd, ".", O_RDWR | O_CLOEXEC, &part->tmpf, error))
    return FALSE;

  g_auto (OtChecksum) checksum = {
    0,
  };
  ot_checksum_init (&checksum);
  PartWriter writer = { part->tmpf.fd, &checksum, 0 };
  {
    /* The header is mostly small integers; compress it with the codec of
     * the effort, at a strong setting since it is small.
     */
    OtDeltaCompressOptions hopts = {
      0,
    };
    hopts.type = self->effort->compression;
    hopts.level = hopts.type == OSTREE_STATIC_DELTA_COMPRESSION_ZSTD ? 19 : 9;
    hopts.src_size_known = TRUE;
    hopts.src_size = g_variant_get_size (header);
    g_autoptr (GByteArray) stored = g_byte_array_new ();
    MemReader reader = { g_variant_get_data (header), g_variant_get_size (header), 0 };
    if (!_ostree_delta_compress (&hopts, mem_reader_read, &reader, bytearray_writer_write, stored,
                                 cancellable, error))
      return FALSE;

    const guint8 prefix[2] = { OSTREE_STATIC_DELTA_PART_BULK, hopts.type };
    const guint64 header_len = GUINT64_TO_LE ((guint64)stored->len);
    if (!part_writer_write (&writer, prefix, sizeof (prefix), error)
        || !part_writer_write (&writer, (const guint8 *)&header_len, sizeof (header_len), error)
        || !part_writer_write (&writer, stored->data, stored->len, error))
      return FALSE;
  }

  copts.src_size_known = TRUE;
  copts.src_size = data_size;

  /* With threads to spare and enough data to matter, write and checksum the
   * output behind the compressor, and read objects ahead of it; see the
   * pipeline helpers.  A single-threaded run stays on one thread.
   */
  const gboolean pipelined = self->threads > 1 && data_size > SMALL_DIFF_BYTES;
  AsyncSink sink;
  OtDeltaWriteFunc out_write = part_writer_write;
  gpointer out_data = &writer;
  if (pipelined)
    {
      async_sink_start (&sink, part_writer_write, &writer);
      out_write = async_sink_write;
      out_data = &sink;
    }

  gboolean ok;
  if (!use_diff)
    {
      ConcatCursor cursor = { new_concat, 0 };
      if (pipelined)
        {
          AsyncSource source;
          async_source_start (&source, concat_cursor_read, &cursor);
          ok = _ostree_delta_compress (&copts, async_source_read, &source, out_write, out_data,
                                       cancellable, error);
          async_source_finish (&source);
        }
      else
        ok = _ostree_delta_compress (&copts, concat_cursor_read, &cursor, out_write, out_data,
                                     cancellable, error);
    }
  else if (mem_diff)
    {
      MemReader reader = { g_bytes_get_data (mem_diff, NULL), g_bytes_get_size (mem_diff), 0 };
      ok = _ostree_delta_compress (&copts, mem_reader_read, &reader, out_write, out_data,
                                   cancellable, error);
    }
  else if (lseek (diff_tmpf.fd, 0, SEEK_SET) < 0)
    ok = glnx_throw_errno_prefix (error, "lseek");
  else
    {
      FdReader reader = { diff_tmpf.fd };
      ok = _ostree_delta_compress (&copts, fd_reader_read, &reader, out_write, out_data,
                                   cancellable, error);
    }
  if (pipelined)
    ok = async_sink_finish (&sink, ok, error);
  if (!ok)
    return FALSE;

  part->compressed_size = writer.size;
  part->uncompressed_size = new_concat->size;

  guint8 digest[OSTREE_SHA256_DIGEST_LEN];
  ot_checksum_get_digest (&checksum, digest, sizeof (digest));
  const gboolean swap = self->options->swap_endian;
  part->header = g_variant_ref_sink (g_variant_new (
      "(u@ay@t@t@ay)",
      swap ? GUINT32_SWAP_LE_BE (OSTREE_DELTAPART_VERSION) : OSTREE_DELTAPART_VERSION,
      ot_gvariant_new_bytearray (digest, sizeof (digest)),
      maybe_swap_u64 (swap, part->compressed_size), maybe_swap_u64 (swap, part->uncompressed_size),
      ot_gvariant_new_bytearray (object_names->data, object_names->len)));

  if (self->options->verbose)
    g_printerr (
        "bulk part %u (%s): objects=%u sources=%u new=%" G_GUINT64_FORMAT " old=%" G_GUINT64_FORMAT
        " %s=%" G_GUINT64_FORMAT " compressed=%" G_GUINT64_FORMAT " (%s) in %.2fs\n",
        part_index + 1, name, new_objects->len, use_diff ? old_objects->len : 0, new_concat->size,
        old_concat->size, !use_diff ? "raw" : (mem_diff ? "diff" : "blockdiff"), data_size,
        part->compressed_size, copts.type == OSTREE_STATIC_DELTA_COMPRESSION_ZSTD ? "zstd" : "xz",
        (g_get_monotonic_time () - start_time) / (double)G_USEC_PER_SEC);

  return TRUE;
}

/* Load what we need to know about new content object @checksum */
static gboolean
generator_load_new_file (BulkGenerator *self, const char *checksum, const char *path,
                         VariantTable *modes, VariantTable *xattrs, StreamObject *out,
                         GCancellable *cancellable, GError **error)
{
  g_autoptr (GFileInfo) finfo = NULL;
  g_autoptr (GVariant) file_xattrs = NULL;
  if (!ostree_repo_load_file (self->repo, checksum, NULL, &finfo, &file_xattrs, cancellable, error))
    return FALSE;

  out->objtype = OSTREE_OBJECT_TYPE_FILE;
  memcpy (out->checksum, checksum, OSTREE_SHA256_STRING_LEN + 1);
  out->path = path;

  const guint32 uid = g_file_info_get_attribute_uint32 (finfo, "unix::uid");
  const guint32 gid = g_file_info_get_attribute_uint32 (finfo, "unix::gid");
  const guint32 mode = g_file_info_get_attribute_uint32 (finfo, "unix::mode");
  g_autoptr (GVariant) modev = g_variant_ref_sink (
      g_variant_new ("(uuu)", GUINT32_TO_BE (uid), GUINT32_TO_BE (gid), GUINT32_TO_BE (mode)));
  out->mode_index = variant_table_intern (modes, modev);
  out->xattr_index = variant_table_intern (xattrs, file_xattrs);

  if (S_ISLNK (mode))
    {
      const char *target = g_file_info_get_symlink_target (finfo);
      out->mem = g_bytes_new (target, strlen (target));
      out->size = g_bytes_get_size (out->mem);
    }
  else if (S_ISREG (mode))
    out->size = g_file_info_get_size (finfo);
  else
    return glnx_throw (error, "Unsupported file type for object %s", checksum);

  return TRUE;
}

/* Describe old regular file @checksum as a diff source; sets *out_usable to
 * FALSE if it should not be used.
 */
static gboolean
generator_load_old_file (BulkGenerator *self, const char *checksum, const char *path,
                         StreamObject *out, gboolean *out_usable, GCancellable *cancellable,
                         GError **error)
{
  g_autoptr (GFileInfo) finfo = NULL;
  if (!ostree_repo_load_file (self->repo, checksum, NULL, &finfo, NULL, cancellable, error))
    return FALSE;
  const guint32 mode = g_file_info_get_attribute_uint32 (finfo, "unix::mode");
  /* As for bsdiff/rollsum sources: by default only use files that every
   * client can read back, whatever its privileges; see private_sources.
   */
  *out_usable = S_ISREG (mode) && g_file_info_get_size (finfo) > 0
                && (self->options->private_sources || (mode & S_IROTH));
  if (!*out_usable)
    return TRUE;
  out->objtype = OSTREE_OBJECT_TYPE_FILE;
  memcpy (out->checksum, checksum, OSTREE_SHA256_STRING_LEN + 1);
  out->path = path;
  out->size = g_file_info_get_size (finfo);
  return TRUE;
}

static gboolean
generator_load_metadata (BulkGenerator *self, OstreeObjectType objtype, const char *checksum,
                         const char *path, StreamObject *out, GError **error)
{
  g_autoptr (GVariant) v = NULL;
  if (!ostree_repo_load_variant (self->repo, objtype, checksum, &v, error))
    return FALSE;
  out->objtype = objtype;
  memcpy (out->checksum, checksum, OSTREE_SHA256_STRING_LEN + 1);
  out->path = path;
  out->mem = g_variant_get_data_as_bytes (v);
  out->size = g_bytes_get_size (out->mem);
  return TRUE;
}

typedef struct
{
  BulkGenerator *gen;
  const char *name;
  guint part_index;
  GArray *new_objects;
  GArray *old_objects;
  gboolean precise;
  VariantTable *modes;
  VariantTable *xattrs;
  GCancellable *cancellable;
  GError *error;
} EmitJob;

static gpointer
emit_job_thread (gpointer data)
{
  EmitJob *job = data;
  return GINT_TO_POINTER (
      generator_emit_part (job->gen, job->name, job->part_index, job->new_objects, job->old_objects,
                           job->precise, job->modes, job->xattrs, job->cancellable, &job->error));
}

void
_ostree_static_delta_bulk_part_free (OstreeStaticDeltaBulkPart *part)
{
  if (part == NULL)
    return;
  glnx_tmpfile_clear (&part->tmpf);
  g_clear_pointer (&part->header, g_variant_unref);
  g_free (part);
}

gboolean
_ostree_static_delta_bulk_generate (OstreeRepo *repo, const char *from, const char *to,
                                    const OstreeStaticDeltaBulkOptions *options, int parts_dfd,
                                    GPtrArray *out_parts, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Generating bulk delta", error);

  BulkGenerator gen = {
    0,
  };
  gen.repo = repo;
  gen.options = options;
  gen.effort = &effort_table[MIN (options->effort, OSTREE_STATIC_DELTA_BULK_EFFORT_MAX)];
  gen.threads = options->threads > 0 ? options->threads : g_get_num_processors ();
  gen.parts_dfd = parts_dfd;
  gen.out_parts = out_parts;
  gen.bare = _ostree_repo_mode_is_bare (ostree_repo_get_mode (repo));

#ifndef HAVE_HDIFFPATCH
  return glnx_throw (error, "This version of ostree was built without hdiffpatch support");
#endif
#ifndef HAVE_ZSTD
  if (gen.effort->compression == OSTREE_STATIC_DELTA_COMPRESSION_ZSTD)
    return glnx_throw (error, "Effort levels below 8 need zstd, which this version of ostree "
                              "was built without");
#endif

  const gint64 start_time = g_get_monotonic_time ();

  g_auto (CommitContents) to_contents = {
    0,
  };
  g_auto (CommitContents) from_contents = {
    0,
  };
  commit_contents_init (&to_contents);
  commit_contents_init (&from_contents);
  if (!walk_commit (repo, to, &to_contents, cancellable, error))
    return FALSE;
  if (from != NULL && !walk_commit (repo, from, &from_contents, cancellable, error))
    return FALSE;

  g_auto (VariantTable) modes = {
    0,
  };
  g_auto (VariantTable) xattrs = {
    0,
  };
  variant_table_init (&modes);
  variant_table_init (&xattrs);

  gboolean ret = FALSE;
  /* "modified" part */
  g_autoptr (GArray) mod_new = stream_object_array_new ();
  g_autoptr (GArray) mod_old = stream_object_array_new ();
  /* "new" part */
  g_autoptr (GArray) add_new = stream_object_array_new ();
  g_autoptr (GArray) add_old = stream_object_array_new ();

  g_autoptr (GStringChunk) names = g_string_chunk_new (4096);

  /* Names (see route_name()) of regular files that are gone in the target */
  g_autoptr (GHashTable) gone_basenames = g_hash_table_new (g_str_hash, g_str_equal);
  /* Names of the new regular files in each part */
  g_autoptr (GHashTable) mod_basenames = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GHashTable) add_basenames = g_hash_table_new (g_str_hash, g_str_equal);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, from_contents.files);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (!g_hash_table_contains (to_contents.files, key))
      g_hash_table_add (gone_basenames, (char *)route_name (names, value));

  /* New metadata.  The commit object itself travels in the superblock. */
  struct
  {
    GHashTable *to;
    GHashTable *from;
    OstreeObjectType objtype;
  } meta_kinds[]
      = { { to_contents.dirtrees, from_contents.dirtrees, OSTREE_OBJECT_TYPE_DIR_TREE },
          { to_contents.dirmetas, from_contents.dirmetas, OSTREE_OBJECT_TYPE_DIR_META } };
  for (guint k = 0; k < G_N_ELEMENTS (meta_kinds); k++)
    {
      g_hash_table_iter_init (&iter, meta_kinds[k].to);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          if (g_hash_table_contains (meta_kinds[k].from, key))
            continue;
          StreamObject obj = {
            0,
          };
          if (!generator_load_metadata (&gen, meta_kinds[k].objtype, key, value, &obj, error))
            goto out;
          g_array_append_val (mod_new, obj);
        }
      g_hash_table_iter_init (&iter, meta_kinds[k].from);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          if (g_hash_table_contains (meta_kinds[k].to, key))
            continue;
          StreamObject obj = {
            0,
          };
          if (!generator_load_metadata (&gen, meta_kinds[k].objtype, key, value, &obj, error))
            goto out;
          g_array_append_val (mod_old, obj);
        }
    }

  /* New content */
  g_hash_table_iter_init (&iter, to_contents.files);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (g_hash_table_contains (from_contents.files, key))
        continue;
      StreamObject obj = {
        0,
      };
      if (!generator_load_new_file (&gen, key, value, &modes, &xattrs, &obj, cancellable, error))
        goto out;
      const char *basename = route_name (names, value);
      /* Symlinks are tiny; keep them with the metadata */
      if (obj.mem != NULL || g_hash_table_contains (gone_basenames, basename))
        {
          if (obj.mem == NULL)
            g_hash_table_add (mod_basenames, (char *)basename);
          g_array_append_val (mod_new, obj);
        }
      else
        {
          g_hash_table_add (add_basenames, (char *)basename);
          g_array_append_val (add_new, obj);
        }
    }

  /* Old content to diff against */
  g_hash_table_iter_init (&iter, from_contents.files);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *basename = route_name (names, value);
      const gboolean gone = !g_hash_table_contains (to_contents.files, key);
      GArray *target = NULL;
      if (gone && g_hash_table_contains (mod_basenames, basename))
        target = mod_old;
      else if (add_new->len > 0 && (gone || g_hash_table_contains (add_basenames, basename)))
        target = add_old;
      if (target == NULL)
        continue;
      StreamObject obj = {
        0,
      };
      gboolean usable;
      if (!generator_load_old_file (&gen, key, value, &obj, &usable, cancellable, error))
        goto out;
      if (usable)
        g_array_append_val (target, obj);
    }

  /* With nothing to diff the metadata against there is no point in a
   * separate part: one stream compresses better.
   */
  gboolean merged = FALSE;
  if (mod_old->len == 0)
    {
      merged = TRUE;
      g_array_append_vals (mod_new, add_new->data, add_new->len);
      /* The entries were moved, not copied */
      g_array_set_clear_func (add_new, NULL);
      g_array_set_size (add_new, 0);
      g_array_append_vals (mod_old, add_old->data, add_old->len);
      g_array_set_clear_func (add_old, NULL);
      g_array_set_size (add_old, 0);
    }

  g_array_sort (mod_new, stream_object_compare);
  g_array_sort (mod_old, stream_object_compare);
  g_array_sort (add_new, stream_object_compare);
  g_array_sort (add_old, stream_object_compare);

  if (options->verbose)
    g_printerr ("bulk delta: effort=%u threads=%u scan=%.2fs\n",
                MIN (options->effort, OSTREE_STATIC_DELTA_BULK_EFFORT_MAX), gen.threads,
                (g_get_monotonic_time () - start_time) / (double)G_USEC_PER_SEC);

  {
    /* If the groups were merged above, the first part is mostly new content,
     * which the precise differ is slow and no better at.
     */
    EmitJob jobs[2]
        = { { &gen, merged ? "all" : "modified", 0, mod_new, mod_old, !merged, &modes, &xattrs,
              cancellable, NULL },
            { &gen, "new", 0, add_new, add_old, FALSE, &modes, &xattrs, cancellable, NULL } };
    guint n_jobs = 0;
    for (guint j = 0; j < G_N_ELEMENTS (jobs); j++)
      {
        if (jobs[j].new_objects->len == 0)
          continue;
        jobs[n_jobs] = jobs[j];
        jobs[n_jobs].part_index = out_parts->len;
        g_ptr_array_add (out_parts, g_new0 (OstreeStaticDeltaBulkPart, 1));
        n_jobs++;
      }

    /* The parts are independent; do them concurrently.  Unpacking a
     * non-bare repository goes through one shared temporary file, though.
     */
    GThread *thread = NULL;
    if (n_jobs == 2 && gen.bare)
      thread = g_thread_new ("ostree-bulk-delta", emit_job_thread, &jobs[1]);
    gboolean ok = TRUE;
    for (guint j = 0; j < n_jobs; j++)
      {
        if (j == 1 && thread != NULL)
          ok = GPOINTER_TO_INT (g_thread_join (thread)) && ok;
        else
          ok = GPOINTER_TO_INT (emit_job_thread (&jobs[j])) && ok;
      }
    for (guint j = 0; j < n_jobs; j++)
      {
        if (jobs[j].error != NULL && error != NULL && *error == NULL)
          g_propagate_error (error, g_steal_pointer (&jobs[j].error));
        g_clear_error (&jobs[j].error);
      }
    if (!ok)
      goto out;
  }

  ret = TRUE;
out:
  glnx_tmpfile_clear (&gen.unpack_tmpf);
  return ret;
}

/* ------------------------------------------------------------------------ */
/* Application                                                               */
/* ------------------------------------------------------------------------ */

typedef struct
{
  guint8 compression;
  guint8 kind;
  guint64 new_size;
  guint64 data_size;
  GVariant *sources;
  GVariant *modes;
  GVariant *xattrs;
  GVariant *entries;
  const guint8 *data;
  gsize data_len;
  GVariant *header;
} BulkPart;

static void
bulk_part_clear (BulkPart *self)
{
  g_clear_pointer (&self->sources, g_variant_unref);
  g_clear_pointer (&self->modes, g_variant_unref);
  g_clear_pointer (&self->xattrs, g_variant_unref);
  g_clear_pointer (&self->entries, g_variant_unref);
  g_clear_pointer (&self->header, g_variant_unref);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (BulkPart, bulk_part_clear)

/* @part is the "ay" variant produced by _ostree_static_delta_part_open() */
static gboolean
bulk_part_parse (GVariant *part, BulkPart *out, GError **error)
{
  gsize len;
  const guint8 *data = g_variant_get_fixed_array (part, &len, 1);
  guint64 header_len;
  const gsize prefix_len = 1 + sizeof (header_len);
  if (len < prefix_len)
    return glnx_throw (error, "Truncated bulk delta part");
  const guint8 header_compression = data[0];
  memcpy (&header_len, data + 1, sizeof (header_len));
  header_len = GUINT64_FROM_LE (header_len);
  if (header_len > len - prefix_len)
    return glnx_throw (error, "Invalid bulk delta part header length");

  g_autoptr (GBytes) header_bytes = NULL;
  {
    g_autoptr (OtDeltaDecompressor) decompressor
        = _ostree_delta_decompressor_new (header_compression, data + prefix_len, header_len, error);
    if (!decompressor)
      return FALSE;
    g_autoptr (GByteArray) buf = g_byte_array_new ();
    while (TRUE)
      {
        const gsize chunk = 64 * 1024;
        const guint old_len = buf->len;
        if (old_len > MAX_HEADER_SIZE)
          return glnx_throw (error, "Bulk delta part header is too large");
        g_byte_array_set_size (buf, old_len + chunk);
        gsize n;
        if (!_ostree_delta_decompressor_read (decompressor, buf->data + old_len, chunk, &n, error))
          return glnx_prefix_error (error, "Reading bulk delta part header");
        g_byte_array_set_size (buf, old_len + n);
        if (n < chunk)
          break;
      }
    header_bytes = g_byte_array_free_to_bytes (g_steal_pointer (&buf));
  }
  out->header = g_variant_ref_sink (g_variant_new_from_bytes (
      G_VARIANT_TYPE (OSTREE_STATIC_DELTA_BULK_HEADER_FORMAT), header_bytes, FALSE));
  if (!g_variant_is_normal_form (out->header))
    return glnx_throw (error, "Invalid bulk delta part header");

  g_variant_get (out->header, "(yytt@ay@a(uuu)@aa(ayay)@a(uut))", &out->compression, &out->kind,
                 &out->new_size, &out->data_size, &out->sources, &out->modes, &out->xattrs,
                 &out->entries);
  out->new_size = GUINT64_FROM_LE (out->new_size);
  out->data_size = GUINT64_FROM_LE (out->data_size);
  out->data = data + prefix_len + header_len;
  out->data_len = len - prefix_len - header_len;

  if (out->kind != OSTREE_STATIC_DELTA_BULK_KIND_RAW
      && out->kind != OSTREE_STATIC_DELTA_BULK_KIND_HDIFF)
    return glnx_throw (error, "Unknown bulk delta part kind %u", out->kind);
  if (g_variant_get_size (out->sources) % OBJ_NAME_LEN != 0)
    return glnx_throw (error, "Invalid bulk delta part source list");
  return TRUE;
}

gboolean
_ostree_static_delta_bulk_part_describe (GVariant *part, char **out_description, GError **error)
{
  g_auto (BulkPart) parsed = {
    0,
  };
  if (!bulk_part_parse (part, &parsed, error))
    return FALSE;
  const char *comp
      = parsed.compression == OSTREE_STATIC_DELTA_COMPRESSION_ZSTD
            ? "zstd"
            : (parsed.compression == OSTREE_STATIC_DELTA_COMPRESSION_XZ ? "xz" : "none");
  *out_description = g_strdup_printf (
      "bulk kind=%s compression=%s objects=%" G_GSIZE_FORMAT " sources=%" G_GSIZE_FORMAT
      " newsize=%" G_GUINT64_FORMAT " datasize=%" G_GUINT64_FORMAT
      " compressedsize=%" G_GSIZE_FORMAT,
      parsed.kind == OSTREE_STATIC_DELTA_BULK_KIND_HDIFF ? "hdiff" : "raw", comp,
      g_variant_n_children (parsed.entries), g_variant_get_size (parsed.sources) / OBJ_NAME_LEN,
      parsed.new_size, parsed.data_size, parsed.data_len);
  return TRUE;
}

typedef struct
{
  OstreeRepo *repo;
  GCancellable *cancellable;
  BulkPart *part;
  const guint8 *object_names;
  guint n_objects;

  /* Current object */
  guint index;
  gboolean started;
  gboolean have_obj;
  OstreeObjectType objtype;
  char checksum[OSTREE_SHA256_STRING_LEN + 1];
  guint64 size;
  guint64 written;
  guint32 uid, gid, mode;
  GVariant *xattrs;
  GByteArray *buffer; /* metadata and symlinks */
  OstreeRepoBareContent content_out;

  GError *error;
} StreamSink;

static void
stream_sink_clear (StreamSink *self)
{
  g_clear_pointer (&self->xattrs, g_variant_unref);
  g_clear_pointer (&self->buffer, g_byte_array_unref);
  _ostree_repo_bare_content_cleanup (&self->content_out);
  g_clear_error (&self->error);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (StreamSink, stream_sink_clear)

static gboolean
stream_sink_begin_object (StreamSink *self, GError **error)
{
  const guint8 *name = self->object_names + (gsize)self->index * OBJ_NAME_LEN;
  if (!ostree_validate_structureof_objtype (name[0], error))
    return FALSE;
  self->objtype = (OstreeObjectType)name[0];
  ostree_checksum_inplace_from_bytes (name + 1, self->checksum);

  guint32 mode_index, xattr_index;
  g_variant_get_child (self->part->entries, self->index, "(uut)", &mode_index, &xattr_index,
                       &self->size);
  mode_index = GUINT32_FROM_LE (mode_index);
  xattr_index = GUINT32_FROM_LE (xattr_index);
  self->size = GUINT64_FROM_LE (self->size);
  self->written = 0;
  self->started = TRUE;
  g_clear_pointer (&self->xattrs, g_variant_unref);
  g_clear_pointer (&self->buffer, g_byte_array_unref);

  if (!ostree_repo_has_object (self->repo, self->objtype, self->checksum, &self->have_obj,
                               self->cancellable, error))
    return FALSE;

  if (OSTREE_OBJECT_TYPE_IS_META (self->objtype))
    {
      if (self->objtype != OSTREE_OBJECT_TYPE_DIR_TREE
          && self->objtype != OSTREE_OBJECT_TYPE_DIR_META)
        return glnx_throw (error, "Unexpected object type %u in bulk delta part", self->objtype);
      if (self->size > OSTREE_MAX_METADATA_SIZE)
        return glnx_throw (error, "Metadata object %s is too large", self->checksum);
      if (!self->have_obj)
        self->buffer = g_byte_array_new ();
      return TRUE;
    }

  if (mode_index >= g_variant_n_children (self->part->modes))
    return glnx_throw (error, "Bulk delta mode index %u out of range", mode_index);
  if (xattr_index >= g_variant_n_children (self->part->xattrs))
    return glnx_throw (error, "Bulk delta xattr index %u out of range", xattr_index);
  g_variant_get_child (self->part->modes, mode_index, "(uuu)", &self->uid, &self->gid, &self->mode);
  self->uid = GUINT32_FROM_BE (self->uid);
  self->gid = GUINT32_FROM_BE (self->gid);
  self->mode = GUINT32_FROM_BE (self->mode);
  self->xattrs = g_variant_get_child_value (self->part->xattrs, xattr_index);

  if (S_ISLNK (self->mode))
    {
      if (self->size > MAX_SYMLINK_TARGET_SIZE)
        return glnx_throw (error, "Symlink target of %s is too long", self->checksum);
      if (!self->have_obj)
        self->buffer = g_byte_array_new ();
    }
  else if (S_ISREG (self->mode))
    {
      if (!self->have_obj
          && !_ostree_repo_bare_content_open (self->repo, self->checksum, self->size, self->uid,
                                              self->gid, self->mode, self->xattrs,
                                              &self->content_out, self->cancellable, error))
        return FALSE;
    }
  else
    return glnx_throw (error, "Invalid mode %o for object %s", self->mode, self->checksum);

  return TRUE;
}

static gboolean
stream_sink_finish_object (StreamSink *self, GError **error)
{
  if (self->have_obj)
    ;
  else if (OSTREE_OBJECT_TYPE_IS_META (self->objtype))
    {
      g_autoptr (GBytes) bytes = g_byte_array_free_to_bytes (g_steal_pointer (&self->buffer));
      g_autoptr (GVariant) metadata = g_variant_ref_sink (
          g_variant_new_from_bytes (ostree_metadata_variant_type (self->objtype), bytes, FALSE));
      g_autofree guchar *actual_csum = NULL;
      if (!ostree_repo_write_metadata (self->repo, self->objtype, self->checksum, metadata,
                                       &actual_csum, self->cancellable, error))
        return FALSE;
    }
  else if (S_ISLNK (self->mode))
    {
      g_autoptr (GFileInfo) finfo
          = _ostree_mode_uidgid_to_gfileinfo (self->mode, self->uid, self->gid);
      g_autofree char *target = g_strndup ((const char *)self->buffer->data, self->buffer->len);
      g_file_info_set_symlink_target (finfo, target);
      g_autoptr (GInputStream) object_input = NULL;
      guint64 objlen;
      if (!ostree_raw_file_to_content_stream (NULL, finfo, self->xattrs, &object_input, &objlen,
                                              self->cancellable, error))
        return FALSE;
      g_autofree guchar *actual_csum = NULL;
      if (!ostree_repo_write_content (self->repo, self->checksum, object_input, objlen,
                                      &actual_csum, self->cancellable, error))
        return FALSE;
    }
  else
    {
      char actual_checksum[OSTREE_SHA256_STRING_LEN + 1];
      if (!_ostree_repo_bare_content_commit (self->repo, &self->content_out, actual_checksum,
                                             sizeof (actual_checksum), self->cancellable, error))
        return FALSE;
      if (strcmp (actual_checksum, self->checksum) != 0)
        return glnx_throw (error, "Corrupted object %s (actual checksum is %s)", self->checksum,
                           actual_checksum);
      _ostree_repo_bare_content_cleanup (&self->content_out);
    }

  self->started = FALSE;
  self->index++;
  return TRUE;
}

/* Start objects, and finish complete ones, until one wants data */
static gboolean
stream_sink_advance (StreamSink *self, GError **error)
{
  while (self->index < self->n_objects)
    {
      if (!self->started && !stream_sink_begin_object (self, error))
        return FALSE;
      if (self->written < self->size)
        break;
      if (!stream_sink_finish_object (self, error))
        return FALSE;
    }
  return TRUE;
}

static gboolean
stream_sink_write_inner (StreamSink *self, const guint8 *buf, gsize len, GError **error)
{
  while (len > 0)
    {
      if (g_cancellable_set_error_if_cancelled (self->cancellable, error))
        return FALSE;
      if (!stream_sink_advance (self, error))
        return FALSE;
      if (self->index >= self->n_objects)
        return glnx_throw (error, "Bulk delta part has trailing data");

      const gsize n = MIN ((guint64)len, self->size - self->written);
      if (self->have_obj)
        ;
      else if (self->buffer)
        g_byte_array_append (self->buffer, buf, n);
      else if (!_ostree_repo_bare_content_write (self->repo, &self->content_out, buf, n,
                                                 self->cancellable, error))
        return FALSE;
      self->written += n;
      buf += n;
      len -= n;
    }
  return TRUE;
}

static gboolean
stream_sink_write (gpointer user_data, const guint8 *buf, gsize len)
{
  StreamSink *self = user_data;
  if (self->error != NULL)
    return FALSE;
  return stream_sink_write_inner (self, buf, len, &self->error);
}

typedef struct
{
  OtDeltaDecompressor *decompressor;
  guint64 pos;
  GError *error;
} DiffReader;

/* HDiffPatch reads the diff strictly front to back */
static gboolean
diff_reader_read (gpointer user_data, guint64 pos, guint8 *buf, gsize len)
{
  DiffReader *self = user_data;
  if (self->error != NULL)
    return FALSE;
  if (pos != self->pos)
    {
      g_set_error (&self->error, G_IO_ERROR, G_IO_ERROR_FAILED, "Non-sequential read of diff");
      return FALSE;
    }
  gsize n;
  if (!_ostree_delta_decompressor_read (self->decompressor, buf, len, &n, &self->error))
    return FALSE;
  if (n != len)
    {
      g_set_error (&self->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Truncated diff data");
      return FALSE;
    }
  self->pos += n;
  return TRUE;
}

gboolean
_ostree_static_delta_bulk_part_execute (OstreeRepo *repo, GVariant *objects, GVariant *part,
                                        gboolean stats_only, GCancellable *cancellable,
                                        GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Executing bulk delta part", error);

  g_auto (BulkPart) parsed = {
    0,
  };
  if (!bulk_part_parse (part, &parsed, error))
    return FALSE;

  guint8 *object_names;
  guint n_objects;
  if (!_ostree_static_delta_parse_checksum_array (objects, &object_names, &n_objects, error))
    return FALSE;
  if (g_variant_n_children (parsed.entries) != n_objects)
    return glnx_throw (error, "Bulk delta part describes %" G_GSIZE_FORMAT " objects, expected %u",
                       g_variant_n_children (parsed.entries), n_objects);
  {
    guint64 total = 0;
    for (guint i = 0; i < n_objects; i++)
      {
        guint64 size;
        g_variant_get_child (parsed.entries, i, "(uut)", NULL, NULL, &size);
        size = GUINT64_FROM_LE (size);
        if (size > G_MAXUINT64 - total)
          return glnx_throw (error, "Invalid object sizes in bulk delta part");
        total += size;
      }
    if (total != parsed.new_size)
      return glnx_throw (error, "Object sizes in bulk delta part do not add up");
  }

  if (stats_only)
    return TRUE;

  if (!_ostree_repo_mode_is_bare (ostree_repo_get_mode (repo)))
    return glnx_throw (error, "Bulk deltas can only be applied to bare repositories");

  g_auto (StreamSink) sink = {
    0,
  };
  sink.repo = repo;
  sink.cancellable = cancellable;
  sink.part = &parsed;
  sink.object_names = object_names;
  sink.n_objects = n_objects;

  g_autoptr (OtDeltaDecompressor) decompressor
      = _ostree_delta_decompressor_new (parsed.compression, parsed.data, parsed.data_len, error);
  if (!decompressor)
    return FALSE;

  if (parsed.kind == OSTREE_STATIC_DELTA_BULK_KIND_RAW)
    {
      g_autofree guint8 *buf = g_malloc (IO_BUF_SIZE);
      guint64 remaining = parsed.new_size;
      while (remaining > 0)
        {
          gsize n;
          if (!_ostree_delta_decompressor_read (decompressor, buf, MIN (remaining, IO_BUF_SIZE), &n,
                                                error))
            return FALSE;
          if (n == 0)
            return glnx_throw (error, "Truncated bulk delta part");
          if (!stream_sink_write_inner (&sink, buf, n, error))
            return FALSE;
          remaining -= n;
        }
    }
  else
    {
#ifdef HAVE_HDIFFPATCH
      /* Assemble the old stream out of objects we must already have */
      g_autoptr (Concat) old_concat = concat_new (repo);
      gsize sources_len;
      const guint8 *sources = g_variant_get_fixed_array (parsed.sources, &sources_len, 1);
      for (gsize i = 0; i < sources_len / OBJ_NAME_LEN; i++)
        {
          const guint8 *name = sources + i * OBJ_NAME_LEN;
          char checksum[OSTREE_SHA256_STRING_LEN + 1];
          ostree_checksum_inplace_from_bytes (name + 1, checksum);
          if (name[0] == OSTREE_OBJECT_TYPE_DIR_TREE || name[0] == OSTREE_OBJECT_TYPE_DIR_META)
            {
              g_autoptr (GVariant) v = NULL;
              if (!ostree_repo_load_variant (repo, (OstreeObjectType)name[0], checksum, &v, error))
                return glnx_prefix_error (error, "Loading delta source");
              g_autoptr (GBytes) bytes = g_variant_get_data_as_bytes (v);
              concat_append_bytes (old_concat, bytes);
            }
          else if (name[0] == OSTREE_OBJECT_TYPE_FILE)
            {
              glnx_autofd int fd = -1;
              struct stat stbuf;
              if (!_ostree_repo_load_file_bare (repo, checksum, &fd, &stbuf, NULL, NULL,
                                                cancellable, error))
                return glnx_prefix_error (error, "Loading delta source");
              if (fd == -1 || !S_ISREG (stbuf.st_mode))
                return glnx_throw (error, "Delta source %s is not a regular file", checksum);
              concat_append_object (old_concat, checksum, stbuf.st_size);
            }
          else
            return glnx_throw (error, "Invalid delta source object type %u", name[0]);
        }

      DiffReader reader = { decompressor, 0, NULL };
      const OtHdiffInput old_in = concat_as_input (old_concat);
      const OtHdiffInput diff_in = { parsed.data_size, diff_reader_read, &reader };
      g_autoptr (GError) patch_error = NULL;
      if (!_ostree_hdiff_patch_stream (&old_in, &diff_in, parsed.new_size, stream_sink_write, &sink,
                                       &patch_error))
        {
          /* Prefer the error that made the patcher fail */
          g_autoptr (GError) reader_error = reader.error;
          if (sink.error != NULL)
            g_propagate_error (error, g_steal_pointer (&sink.error));
          else if (old_concat->error != NULL)
            g_propagate_error (error, g_steal_pointer (&old_concat->error));
          else if (reader_error != NULL)
            g_propagate_error (error, g_steal_pointer (&reader_error));
          else
            g_propagate_error (error, g_steal_pointer (&patch_error));
          return FALSE;
        }
      g_clear_error (&reader.error);
#else
      return glnx_throw (error, "This version of ostree was built without hdiffpatch support");
#endif
    }

  /* Finish the last object, and any trailing empty ones */
  if (!stream_sink_advance (&sink, error))
    return FALSE;
  if (sink.index != n_objects)
    return glnx_throw (error, "Bulk delta part ended early");

  return TRUE;
}
