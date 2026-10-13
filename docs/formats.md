---
nav_order: 80
---

# OSTree data formats
{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

## On the topic of "smart servers"

One really crucial difference between OSTree and git is that git has a
"smart server".  Even when fetching over `https://`, it isn't just a
static webserver, but one that e.g. dynamically computes and
compresses pack files for each client.

In contrast, the author of OSTree feels that for operating system
updates, many deployments will want to use simple static webservers,
the same target most package systems were designed to use.  The
primary advantages are security and compute efficiency.  Services like
Amazon S3 and CDNs are a canonical target, as well as a stock static
nginx server.

## The archive format

In the [repo](repo.md) section, the concept of objects was introduced,
where file/content objects are checksummed and managed individually.
(Unlike a package system, which operates on compressed aggregates).

The `archive` format simply gzip-compresses each content object.
Metadata objects are stored uncompressed.  This means that it's easy
to serve via static HTTP.  Note: the repo config file still uses the
historical term `archive-z2` as mode. But this essentially indicates
the modern `archive` format.

When you commit new content, you will see new `.filez` files appearing
in `objects/`.

## archive efficiency

The advantages of `archive`:

 - It's easy to understand and implement
 - Can be served directly over plain HTTP by a static webserver
 - Clients can download/unpack updates incrementally
 - Space efficient on the server

The biggest disadvantage of this format is that for a client to
perform an update, one HTTP request per changed file is required.  In
some scenarios, this actually isn't bad at all, particularly with
techniques to reduce HTTP overhead, such as
[HTTP/2](https://en.wikipedia.org/wiki/HTTP/2).

In order to make this format work well, you should design your content
such that large data that changes infrequently (e.g. graphic images)
are stored separately from small frequently changing data (application
code).

Other disadvantages of `archive`:

 - It's quite bad when clients are performing an initial pull (without HTTP/2),
 - One doesn't know the total size (compressed or uncompressed) of content
   before downloading everything

## Aside: bare formats

The most common operation is to pull from a remote `archive` repository
into a local one.  This latter is not compressed on disk.  In other
words, pulling to a local repository is similar to unpacking (but not
installing) the content of an RPM/deb package.

The `bare` repository format is the simplest one. In this mode regular files
are directly stored to disk, and all metadata (e.g. uid/gid and xattrs) is
reflected to the filesystem.
It allows further direct access to content and metadata, but it may require
elevated privileges when writing objects to the repository.

The `bare-user` format is a bit special in that the uid/gid and xattrs
from the content are ignored.  This is primarily useful if you want to
have the same OSTree-managed content that can be run on a host system
or an unprivileged container.

Similarly, the `bare-split-xattrs` format is a special mode where xattrs
are stored as separate repository objects, and not directly reflected to
the filesystem.
This is primarily useful when transporting xattrs through lossy environments
(e.g. tar streams and containerized environments). It also allows carrying
security-sensitive xattrs (e.g. SELinux labels) out-of-band without involving
OS filesystem logic.

## Static deltas

OSTree itself was originally focused on a continuous delivery model, where
client systems are expected to update regularly.  However, many OS vendors
would like to supply content that's updated e.g. once a month or less often.

For this model, we can do a lot better to support batched updates than
a basic `archive` repo. However, we still want to preserve the
model of "static webserver only".  Given this, OSTree has gained the
concept of a "static delta".

These deltas are targeted to be a delta between two specific commit
objects, including "bsdiff" and "rsync-style" deltas within a content
object.  Static deltas also support `from NULL`, where the client can
more efficiently download a commit object from scratch - this is
mostly useful when using OSTree for containers, rather than OS images.
For OS images, one tends to download an installer ISO or qcow2 image
which is a single file that contains the tree data already.

Effectively, we're spending server-side storage (and one-time compute
cost), and gaining efficiency in client network bandwidth.

## Static delta repository layout

Since static deltas may not exist, the client first needs to attempt
to locate one.  Suppose a client wants to retrieve commit `${new}`
while currently running `${current}`.

In order to save space, these two commits are "modified base64" - the 
`/` character is replaced with `_`.

Like the commit objects, a "prefix directory" is used to make
management easier for filesystem tools.

A delta is named `$(mbase64 $from)-$(mbase64 $to)`, for example
`GpTyZaVut2jXFPWnO4LJiKEdRTvOw_mFUCtIKW1NIX0-L8f+VVDkEBKNc1Ncd+mDUrSVR4EyybQGCkuKtkDnTwk`,
which in SHA256 format is
`1a94f265a56eb768d714f5a73b82c988a11d453bcec3f985502b48296d4d217d-2fc7fe5550e410128d73535c77e98352b495478132c9b4060a4b8ab640e74f09`.

Finally, the actual content can be found in
`deltas/$fromprefix/$fromsuffix-$to`.

## Static delta internal structure

A delta is itself a directory.  Inside, there is a file called
`superblock` which contains metadata.  The rest of the files will be
integers bearing packs of content.

The file format of static deltas should be currently considered an
OSTree implementation detail.  Obviously, nothing stops one from
writing code which is compatible with OSTree today.  However, we would
like the flexibility to expand and change things, and having multiple
codebases makes that more problematic.  Please contact the authors
with any requests.

That said, one critical thing to understand about the design is that
delta payloads are a bit more like "restricted programs" than they are
raw data.  There's a "compilation" phase which generates output that
the client executes.

This "updates as code" model allows for multiple content generation
strategies.  The design of this was inspired by that of Chromium:
[ChromiumOS Autoupdate](http://dev.chromium.org/chromium-os/chromiumos-design-docs/filesystem-autoupdate).

### The delta superblock

The superblock contains:

 - arbitrary metadata
 - delta generation timestamp
 - the new commit object
 - An array of recursive deltas to apply
 - An array of per-part metadata, including total object sizes (compressed and uncompressed),
 - An array of fallback objects

Let's define a delta part, then return to discuss details:

## A delta part

A delta part is a combination of a raw blob of data, plus a very
restricted bytecode that operates on it.  Say for example two files
happen to share a common section.  It's possible for the delta
compilation to include that section once in the delta data blob, then
generate instructions to write out that blob twice when generating
both objects.

Realistically though, it's very common for most of a delta to just be
"stream of new objects" - if one considers it, it doesn't make sense
to have too much duplication inside operating system content at this
level.

So then, what's more interesting is that OSTree static deltas support
a per-file delta algorithm called
[bsdiff](https://github.com/mendsley/bsdiff) that most notably works
well on executable code.

The current delta compiler scans for files with matching basenames in
each commit that have a similar size, and attempts a bsdiff between
them.  (It would make sense later to have a build system provide a
hint for this - for example, files within a same package).

A generated bsdiff is included in the payload blob, and applying it is
an instruction.

### hdiff and zstd

Two optional variations of the above exist, selected when generating
a delta.  Both need a client built with the corresponding support
(`ostree --version` lists `hdiffpatch` and `zstd`); older clients fail
cleanly on the unknown instruction or compression type.

 - `--file-delta=hdiff` uses
   [HDiffPatch](https://github.com/sisong/HDiffPatch) instead of bsdiff
   for the per-file deltas.  bsdiff typically accounts for nearly all
   of the time taken to generate a delta for an operating system
   update; hdiff is around a hundred times faster on the same files,
   produces slightly smaller parts, and is not subject to
   `--max-bsdiff-size`.
 - `--compression=zstd[:LEVEL]` compresses delta parts with zstd
   instead of xz (`--compression=xz:LEVEL` selects an xz preset).  Generating a `from NULL` delta is almost entirely
   compression; zstd makes that an order of magnitude faster at the
   cost of deltas around a third larger.

Independently of these, `--threads=N` compresses several delta parts
at once.  The delta produced is identical; only the time and memory
needed to generate it change.

## Bulk deltas

`ostree static-delta generate --bulk` produces a different kind of
delta part.  Instead of a blob plus instructions and a delta per file,
all of the objects of a part are concatenated into one *stream*:
directory metadata objects first, then file content, each ordered by
path.  The part then carries either that stream, compressed, or a
single HDiffPatch diff that produces it from an *old* stream: the
concatenation of a list of objects the client already has.

Diffing whole groups of objects at once has three effects:

 - Generation is fast.  One pass over the changed data replaces
   thousands of per-file deltas, and identical regions are skipped at
   over a gigabyte per second.
 - Directory metadata is diffed too.  In the classic scheme new
   `dirtree` objects are shipped whole, which for a small update is
   often most of the delta.
 - Files that moved are still matched.  Content is paired by group,
   not by path, so a rebuilt binary inside a renamed directory (for
   example one whose name contains a build hash) diffs against its
   previous version.

A bulk delta has at most two parts, generated concurrently:

 - The *modified* part holds the new metadata objects and the new
   files whose basename matches a file that is gone from the target
   commit.  It is diffed against the metadata and files that are gone,
   using HDiffPatch's precise in-memory differ.  For an ordinary
   update this part is the entire delta.
 - The *new* part holds all other new files, diffed against old files
   sharing a basename with HDiffPatch's low-memory block-matching
   differ.  This is mostly content with nothing to diff against, so
   its cost is compression.

For a `from NULL` delta there is nothing to diff, and a single part
simply holds the compressed stream.

`--effort=N` chooses the tradeoff between generation time and size,
from 0 (as fast as possible, for development loops) to 9 (as small as
possible, for expensive links); the default is 5.  Efforts 0-4
compress with zstd, which is also several times faster to apply;
efforts 5-9 compress with xz.  Compression is multi-threaded; use
`--threads` to limit it.  For an ordinary update the effort barely
matters, since the diff is tiny either way; it matters when there is
a lot of new content.

When a bulk part is applied, the old stream is read by random access
directly from the repository's objects, the diff is decompressed as it
is consumed, and each object is written out and checksummed as the new
stream is produced.  Memory use is small and independent of the size
of the delta.

Bulk deltas use the same superblock as other deltas, so signing,
inline parts, `ostree pull` and `ostree static-delta apply-offline`
all work unchanged.  Their limitations:

 - They need a client built with hdiffpatch support (and zstd support,
   for efforts below 5).
 - They can only be applied to `bare`, `bare-user` and
   `bare-user-only` repositories, because the old stream is read
   directly from the stored objects.
 - They never contain fallback objects, and the options tuning the
   classic scheme (`--min-fallback-size`, `--max-bsdiff-size`,
   `--max-chunk-size`, `--disable-bsdiff`) do not apply.

### Private delta sources

No kind of delta, by default, is computed against a file of the origin
commit that is not world-readable; the new version of such a file is
included whole.  This guarantees that the client can read every object
it needs to apply the delta, even an unprivileged user whose repository
has a root-owned `bare` repository as its parent.

`--allow-private-sources` lifts the restriction.  It is safe wherever
updates are applied by root, or to a `bare-user` repository without such
a parent, and can make a small delta several times smaller: the files
that change in every build of an operating system (package caches,
logs) are often exactly the ones that are not world-readable.

### The bulk part format

A bulk part starts with the byte `b` where other parts have their
compression type, followed by:

 - `y`: the compression of the header
 - `t`: the stored length of the header (little endian)
 - the header, a (compressed) GVariant of type
   `(yyttaya(uuu)aa(ayay)a(uut))`
 - the (compressed) data, to the end of the part

The header contains:

 - `y`: the compression of the data: `0` none, `x` xz, `z` zstd
 - `y`: the kind of data: `0` the new stream itself, `1` an
   uncompressed HDiffPatch single-stream diff (`HDIFFSF20`)
 - `t`: the size of the new stream
 - `t`: the uncompressed size of the data
 - `ay`: the old stream, as a list of (object type, checksum) pairs
 - `a(uuu)`, `aa(ayay)`: modes and extended attributes, as in other
   parts
 - `a(uut)`: for each object of the part, in the order of the part's
   object list in the superblock: a mode index, an xattr index, and
   its size in the new stream

Metadata objects are represented in a stream by their serialized
bytes, regular files by their content, and symbolic links by their
target.

## Fallback objects

It's possible for there to be large-ish files which might be resistant
to bsdiff.  A good example is that it's common for operating systems
to use an "initramfs", which is itself a compressed filesystem.  This
"internal compression" defeats bsdiff analysis.

For these types of objects, the delta superblock contains an array of
"fallback objects".  These objects aren't included in the delta
parts - the client simply fetches them from the underlying `.filez`
object.
