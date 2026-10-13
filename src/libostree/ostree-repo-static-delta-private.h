/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
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

#include "libglnx.h"
#include "ostree-core.h"

G_BEGIN_DECLS

/* 1 byte for object type, 32 bytes for checksum */
#define OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN 33

#define OSTREE_SUMMARY_STATIC_DELTAS "ostree.static-deltas"

/**
 * OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0:
 *
 *   y  compression type (0: none, 'x': lzma)
 *   ---
 *   a(uuu) modes
 *   aa(ayay) xattrs
 *   ay raw data source
 *   ay operations
 */
#define OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0 "(a(uuu)aa(ayay)ayay)"

/**
 * OSTREE_STATIC_DELTA_META_ENTRY_FORMAT:
 *
 *   u: version     (non-canonical endian)
 *   ay checksum
 *   guint64 size:   Total size of delta (sum of parts) (non-canonical endian)
 *   guint64 usize:   Uncompressed size of resulting objects on disk (non-canonical endian)
 *   ARRAY[(guint8 objtype, csum object)]
 *
 * The checksum is of the delta payload, and each entry in the array
 * represents an OSTree object which will be created by the deltapart.
 */

#define OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "(uayttay)"

/**
 * OSTREE_STATIC_DELTA_FALLBACK_FORMAT:
 *
 * y: objtype
 * ay: checksum
 * t: compressed size (non-canonical endian)
 * t: uncompressed size (non-canonical endian)
 *
 * Object to fetch invididually; includes compressed/uncompressed size.
 */
#define OSTREE_STATIC_DELTA_FALLBACK_FORMAT "(yaytt)"

/**
 * OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT:
 *
 * A .delta object is a custom binary format.  It has the following high
 * level form:
 *
 * delta-descriptor:
 *   metadata: a{sv}
 *   t: timestamp (big endian)
 *   from: ay checksum
 *   to: ay checksum
 *   commit: new commit object
 *   ARRAY[(csum from, csum to)]: ay
 *   ARRAY[delta-meta-entry]
 *   array[fallback]
 *
 * The metadata would include things like a version number, as well as
 * extended verification data like a GPG signature.
 *
 * The second array is an array of delta objects that should be
 * fetched and applied before this one.  This is a fairly generic
 * recursion mechanism that would potentially allow saving significant
 * storage space on the server.
 *
 * The heart of the static delta: the array of delta parts.
 *
 * Finally, we have the fallback array, which is the set of objects to
 * fetch individually - the compiler determined it wasn't worth
 * duplicating the space.
 */
#define OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT \
  "(a{sv}tayay" OSTREE_COMMIT_GVARIANT_STRING "aya" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT \
  "a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT ")"

/**
 * OSTREE_STATIC_DELTA_SIGNED_FORMAT
 *
 *   magic: t magic number, 8 bytes for alignment
 *   superblock: ay delta supeblock variant
 *   signatures: a{sv}
 *
 * The signed static delta starts with the 'OSTSGNDT' magic number followed by
 * the array of bytes containing the superblock used for the signature.
 *
 * Then, the signatures array contains the signatures of the superblock. A
 * signature has the following form:
 *  type: signature key
 *  signature: variant depending on type used
 */
#define OSTREE_STATIC_DELTA_SIGNED_FORMAT "(taya{sv})"

#define OSTREE_STATIC_DELTA_SIGNED_MAGIC 0x4F535453474E4454 /* OSTSGNDT */

typedef enum
{
  OSTREE_STATIC_DELTA_OPEN_FLAGS_NONE = 0,
  OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM = (1 << 0),
  OSTREE_STATIC_DELTA_OPEN_FLAGS_VARIANT_TRUSTED = (1 << 1)
} OstreeStaticDeltaOpenFlags;

typedef enum
{
  OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE = 'S',
  OSTREE_STATIC_DELTA_OP_OPEN = 'o',
  OSTREE_STATIC_DELTA_OP_WRITE = 'w',
  OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE = 'r',
  OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE = 'R',
  OSTREE_STATIC_DELTA_OP_CLOSE = 'c',
  OSTREE_STATIC_DELTA_OP_BSPATCH = 'B',
  /* Like BSPATCH, but the payload is an uncompressed HDiffPatch
   * single-stream diff.  Only emitted when explicitly requested.
   */
  OSTREE_STATIC_DELTA_OP_HPATCH = 'H'
} OstreeStaticDeltaOpCode;
#define OSTREE_STATIC_DELTA_N_OPS 8

/* Values of the leading compression byte of a delta part */
#define OSTREE_STATIC_DELTA_COMPRESSION_NONE 0
#define OSTREE_STATIC_DELTA_COMPRESSION_XZ 'x'
#define OSTREE_STATIC_DELTA_COMPRESSION_ZSTD 'z'
#define OSTREE_STATIC_DELTA_ZSTD_DEFAULT_LEVEL 10

/**
 * Bulk delta parts
 *
 * A delta part whose leading byte is OSTREE_STATIC_DELTA_PART_BULK does
 * not use OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0.  Instead its objects,
 * in the order of the part's object array, are concatenated into one "new"
 * stream (metadata objects as their serialized bytes, regular files as their
 * content, symlinks as their target), and the part carries either that
 * stream itself or an HDiffPatch single-stream diff producing it from an
 * "old" stream: the concatenation of a list of objects the client is
 * expected to already have.  Layout after the leading byte:
 *
 *   y                  compression of the header
 *   t                  stored length of the header (little endian)
 *   header             (compressed) OSTREE_STATIC_DELTA_BULK_HEADER_FORMAT
 *   ...                the (compressed) data, to the end of the part
 *
 * Header:
 *   y        compression of the data: an OSTREE_STATIC_DELTA_COMPRESSION_* value
 *   y        kind: OSTREE_STATIC_DELTA_BULK_KIND_*
 *   t        size of the new stream (little endian)
 *   t        uncompressed size of the data (little endian)
 *   ay       old stream sources: (guint8 objtype, csum object) each
 *   a(uuu)   modes, as in OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0
 *   aa(ayay) xattrs, likewise
 *   a(uut)   per new object: mode index, xattr index, size in the stream
 *            (little endian; the indexes are 0 for metadata objects)
 */
#define OSTREE_STATIC_DELTA_PART_BULK 'b'
#define OSTREE_STATIC_DELTA_BULK_HEADER_FORMAT "(yyttaya(uuu)aa(ayay)a(uut))"
#define OSTREE_STATIC_DELTA_BULK_KIND_RAW 0
#define OSTREE_STATIC_DELTA_BULK_KIND_HDIFF 1

#define OSTREE_STATIC_DELTA_BULK_EFFORT_MAX 9
#define OSTREE_STATIC_DELTA_BULK_EFFORT_DEFAULT 5

gboolean _ostree_static_delta_part_open (GInputStream *part_in, GBytes *inline_part_bytes,
                                         OstreeStaticDeltaOpenFlags flags,
                                         const char *expected_checksum, GVariant **out_part,
                                         GCancellable *cancellable, GError **error);

typedef struct
{
  guint n_ops_executed[OSTREE_STATIC_DELTA_N_OPS];
} OstreeDeltaExecuteStats;

gboolean _ostree_static_delta_part_execute (OstreeRepo *repo, GVariant *header,
                                            GVariant *part_payload, gboolean stats_only,
                                            OstreeDeltaExecuteStats *stats,
                                            GCancellable *cancellable, GError **error);

void _ostree_static_delta_part_execute_async (OstreeRepo *repo, GVariant *header,
                                              GVariant *part_payload, GCancellable *cancellable,
                                              GAsyncReadyCallback callback, gpointer user_data);

gboolean _ostree_static_delta_part_execute_finish (OstreeRepo *repo, GAsyncResult *result,
                                                   GError **error);

typedef struct
{
  /* 0 (fastest) to OSTREE_STATIC_DELTA_BULK_EFFORT_MAX (smallest) */
  guint effort;
  /* 0: one per processor */
  guint threads;
  gboolean verbose;
  gboolean swap_endian;
} OstreeStaticDeltaBulkOptions;

typedef struct
{
  GLnxTmpfile tmpf;
  /* OSTREE_STATIC_DELTA_META_ENTRY_FORMAT */
  GVariant *header;
  guint64 compressed_size;
  guint64 uncompressed_size;
} OstreeStaticDeltaBulkPart;

void _ostree_static_delta_bulk_part_free (OstreeStaticDeltaBulkPart *part);

gboolean _ostree_static_delta_bulk_generate (OstreeRepo *repo, const char *from, const char *to,
                                               const OstreeStaticDeltaBulkOptions *options,
                                               int parts_dfd, GPtrArray *out_parts,
                                               GCancellable *cancellable, GError **error);

gboolean _ostree_static_delta_bulk_part_execute (OstreeRepo *repo, GVariant *objects,
                                                   GVariant *part, gboolean stats_only,
                                                   GCancellable *cancellable, GError **error);

gboolean _ostree_static_delta_bulk_part_describe (GVariant *part, char **out_description,
                                                    GError **error);

gboolean _ostree_static_delta_parse_checksum_array (GVariant *array, guint8 **out_checksums_array,
                                                    guint *out_n_checksums, GError **error);

gboolean _ostree_repo_static_delta_part_have_all_objects (OstreeRepo *repo,
                                                          GVariant *checksum_array,
                                                          gboolean *out_have_all,
                                                          GCancellable *cancellable,
                                                          GError **error);

typedef struct
{
  char *checksum;
  guint64 size;
  GPtrArray *basenames;
} OstreeDeltaContentSizeNames;

void _ostree_delta_content_sizenames_free (gpointer v);

gboolean _ostree_delta_compute_similar_objects (OstreeRepo *repo, GVariant *from_commit,
                                                GVariant *to_commit,
                                                GHashTable *new_reachable_regfile_content,
                                                guint similarity_percent_threshold,
                                                GHashTable **out_modified_regfile_content,
                                                GCancellable *cancellable, GError **error);

gboolean _ostree_repo_static_delta_query_exists (OstreeRepo *repo, const char *delta_id,
                                                 gboolean *out_exists, GCancellable *cancellable,
                                                 GError **error);
GVariant *_ostree_repo_static_delta_superblock_digest (OstreeRepo *repo, const char *from,
                                                       const char *to, GCancellable *cancellable,
                                                       GError **error);

gboolean _ostree_repo_static_delta_dump (OstreeRepo *repo, const char *delta_id,
                                         GCancellable *cancellable, GError **error);

gboolean _ostree_repo_static_delta_delete (OstreeRepo *repo, const char *delta_id,
                                           GCancellable *cancellable, GError **error);
gboolean _ostree_repo_static_delta_reindex (OstreeRepo *repo, const char *opt_to_commit,
                                            GCancellable *cancellable, GError **error);

/* Used for static deltas which due to a historical mistake are
 * inconsistent endian.
 *
 * https://bugzilla.gnome.org/show_bug.cgi?id=762515
 */
static inline guint32
maybe_swap_endian_u32 (gboolean swap, guint32 v)
{
  if (!swap)
    return v;
  return GUINT32_SWAP_LE_BE (v);
}

static inline guint64
maybe_swap_endian_u64 (gboolean swap, guint64 v)
{
  if (!swap)
    return v;
  return GUINT64_SWAP_LE_BE (v);
}

typedef enum
{
  OSTREE_DELTA_ENDIAN_BIG,
  OSTREE_DELTA_ENDIAN_LITTLE,
  OSTREE_DELTA_ENDIAN_INVALID
} OstreeDeltaEndianness;

OstreeDeltaEndianness _ostree_delta_get_endianness (GVariant *superblock,
                                                    gboolean *out_was_heuristic);

gboolean _ostree_delta_needs_byteswap (GVariant *superblock);

G_END_DECLS
