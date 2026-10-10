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

#define OSTREE_TYPE_ZSTD_COMPRESSOR (_ostree_zstd_compressor_get_type ())
#define OSTREE_ZSTD_COMPRESSOR(o) \
  (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_ZSTD_COMPRESSOR, OstreeZstdCompressor))
#define OSTREE_IS_ZSTD_COMPRESSOR(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_ZSTD_COMPRESSOR))

typedef struct _OstreeZstdCompressorClass OstreeZstdCompressorClass;
typedef struct _OstreeZstdCompressor OstreeZstdCompressor;

struct _OstreeZstdCompressorClass
{
  GObjectClass parent_class;
};

GType _ostree_zstd_compressor_get_type (void);

OstreeZstdCompressor *_ostree_zstd_compressor_new (int level);

G_END_DECLS
