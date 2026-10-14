# Native `s3://` pull support for libostree

Status: design approved in conversation, awaiting written-spec review
Date: 2026-10-09
Base: upstream ostree `fdef7e6`
Scope: private fork patch (not yet shaped for upstream submission)

## 1. Goal

`ostree remote add foo s3://bucket/repo` followed by `ostree pull foo <ref>`
works against a private S3 bucket using whatever AWS credentials the host
already has. No keys are stored in ostree configuration.

### In scope

- Pull only (ostree has no push; publishing to the bucket stays outside ostree).
- AWS commercial and GovCloud partitions.
- Credential sources: EC2 instance role (IMDS), EKS IRSA, ECS task role,
  shared-config profiles (static keys, assume-role, `credential_process`),
  IAM Identity Center (SSO) profiles, static environment keys.
- The curl fetcher backend only.
- Performance work so S3 pulls are not limited to 8 HTTP/1.1 connections.

### Out of scope

- Writing to S3 from ostree.
- China partition (`amazonaws.com.cn`) and other partitions.
- SSO access-token refresh (see 5.3).
- Remapping 403 to not-found (see 6).
- soup2 / soup3 backends: they reject `s3://`.
- Upstream submission. The design keeps the feature build-time optional so
  this stays possible later.

## 2. Decisions and their reasons

| Decision | Reason |
|---|---|
| `aws-c-auth` resolves credentials **and** signs; curl only transports | AWS's own signer handles session tokens and S3 path encoding correctly, and removes any dependence on the curl version's SigV4 behaviour. |
| Hand-assembled credential chain | The library's default chain omits SSO. Nix's S3 store (`src/libstore/aws-creds.cc`) does the same thing with the same order. |
| Region via `?region=` URL parameter, then AWS defaults | De facto convention (Go CDK, Arrow, go-getter, Nix). ostree already carries the query string through to per-object URLs (`ostree-fetcher-uri.c:97`), so `contenturl` and mirrors each carry their own region. |
| No new remote config keys | Everything S3-specific lives on the URL. |
| 403 is always a hard error | Keeps real permission and credential failures loud. The pulling role must have `s3:GetObject` and `s3:ListBucket` so S3 returns true 404s. |
| Redirects not followed for `s3://` | curl forwards custom headers to redirect targets, which would leak the signature and session token. |
| Build-time optional (`--with-aws`) | The fork still builds without the AWS libraries; `http(s)://` pulls pay nothing. |

## 3. URL surface

```
s3://<bucket>/<prefix>[?region=<r>][&profile=<p>][&endpoint=<url>]
```

- Allowed query parameters: `region`, `profile`, `endpoint`. Any other
  parameter, any fragment, or any userinfo is a validation error.
- **Region resolution order:** `?region=` → `AWS_REGION` →
  `AWS_DEFAULT_REGION` → `region` of the active profile → IMDS → error.
- **Profile resolution order:** `?profile=` → `AWS_PROFILE` → `default`.
- **Addressing:**
  - Default, virtual-hosted: `https://<bucket>.s3.<region>.amazonaws.com/<key>`.
    Identical form for commercial and GovCloud regions.
  - Bucket name contains a dot: path-style,
    `https://s3.<region>.amazonaws.com/<bucket>/<key>`, because the dotted
    hostname fails TLS wildcard matching.
  - `?endpoint=<scheme>://<host>[:<port>]`: always path-style against that
    host. `http` and `https` are both accepted. Used by the local test
    server; also enables FIPS and VPC endpoints.
- **Key encoding:** each path segment is percent-encoded exactly once using
  the SigV4 unreserved set (`A-Z a-z 0-9 - . _ ~`). `/` separators are kept.
  In particular `+` becomes `%2B`, which fixes static-delta paths on S3
  (upstream issue #2139).
- The query string is consumed by the fetcher and never sent to S3.
- Existing remote options `proxy`, `tls-ca-path`, `tls-permissive`,
  `http-headers`, `max-outstanding-fetcher-requests` etc. keep working,
  since curl remains the transport.

## 4. Architecture

### 4.1 Units

| Unit | Purpose | Depends on |
|---|---|---|
| `src/libostree/ostree-fetcher-s3.{c,h}` (new) | Pure logic. Parse an `s3://` URI; build the HTTPS request URL; extract `<Code>`/`<Message>`/`<Region>` from an S3 error XML body. No I/O, no AWS library. | GLib |
| `src/libostree/ostree-aws.{c,h}` (new) | Process-wide AWS runtime. Credential chain and cache, region fallback, SigV4 signing. | `aws-c-auth` and its dependencies |
| `src/libostree/ostree-fetcher-curl.c` (modified) | For `s3://` requests: obtain credentials, rewrite URL, sign, attach headers, map S3 errors. | both units above |
| `src/libostree/ostree-fetcher-uri.c` (modified) | Accept the `s3` scheme when built with AWS support. | — |
| `src/libostree/ostree-repo-pull.c` (modified) | Status message, S3 concurrency default. | — |

Both new units compile only under `--with-aws`. Without it, or when the
active backend is soup, `_ostree_fetcher_uri_validate` rejects `s3://` with:
`this libostree was built without S3 support`.

### 4.2 `ostree-fetcher-s3.h` interface

```c
typedef struct {
  char *bucket;
  char *key_prefix;   /* decoded, no leading slash */
  char *region;       /* from ?region=, or NULL */
  char *profile;      /* from ?profile=, or NULL */
  char *endpoint;     /* from ?endpoint=, or NULL */
} OstreeS3Location;

/* Parse and validate an s3:// URI. */
OstreeS3Location *_ostree_s3_location_parse (GUri *uri, GError **error);
void _ostree_s3_location_free (OstreeS3Location *loc);

/* Build the HTTPS (or endpoint-scheme) URL to request. @region must be
 * resolved (non-NULL) unless loc->endpoint is set. */
char *_ostree_s3_location_to_request_url (const OstreeS3Location *loc,
                                          const char *region);

/* Extract fields from an S3 XML error body. Any out param may come back
 * NULL when the element is absent. Returns FALSE if @body is not an S3
 * error document. */
gboolean _ostree_s3_parse_error_body (const char *body, gsize len,
                                      char **out_code, char **out_message,
                                      char **out_region);
```

XML extraction uses `GMarkupParseContext`, already used for metalinks.

### 4.3 `ostree-aws.h` interface

```c
typedef struct OstreeAwsCredentials OstreeAwsCredentials; /* refcounted, immutable */

OstreeAwsCredentials *_ostree_aws_credentials_ref (OstreeAwsCredentials *c);
void _ostree_aws_credentials_unref (OstreeAwsCredentials *c);
/* Seconds since epoch; G_MAXUINT64 for non-expiring credentials. */
guint64 _ostree_aws_credentials_get_expiry (OstreeAwsCredentials *c);

/* Resolve region using the order in section 3, excluding the URL
 * parameter (the caller passes that in as @url_region). Blocking only
 * for the IMDS fallback. */
char *_ostree_aws_resolve_region (const char *url_region, const char *profile,
                                  GCancellable *cancellable, GError **error);

/* Async. Completes on the thread-default main context of the caller.
 * @force_refresh bypasses the cache (used after an expired-token 403). */
void _ostree_aws_get_credentials_async (const char *profile, const char *region,
                                        gboolean force_refresh,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);
OstreeAwsCredentials *_ostree_aws_get_credentials_finish (GAsyncResult *result,
                                                          GError **error);

/* Synchronous. Signs a bodyless GET for @url and appends the resulting
 * headers ("Name: value") to @headers. */
gboolean _ostree_aws_sign_get (OstreeAwsCredentials *creds, const char *url,
                               const char *region, GPtrArray *headers,
                               GError **error);
```

Runtime internals:

- Started lazily, once per process, on first use (`g_once`). Never torn
  down; the process exits with it.
- One AWS event-loop thread, a host resolver, a client bootstrap, and one
  TLS context shared by the SSO, web-identity and ECS providers.
- A table of cached providers keyed by `(profile, region)`. Region is part
  of the key because the web-identity provider takes it at construction.
- AWS library callbacks arrive on the AWS thread; results are handed to the
  caller's `GMainContext` via `g_main_context_invoke`.

### 4.4 Credential chain

Built per `(profile, region)`, in order, each skipped if its constructor
returns NULL:

1. Environment (`AWS_ACCESS_KEY_ID`, …)
2. SSO (`aws_credentials_provider_new_sso`; NULL when the profile has no SSO
   configuration)
3. Profile (static keys, `role_arn` with `source_profile` or
   `credential_source`, `credential_process`)
4. STS web identity (IRSA), with the resolved region passed explicitly
5. ECS container credentials when its environment variables are set,
   otherwise IMDS unless `AWS_EC2_METADATA_DISABLED=true`

The chain is wrapped in `aws_credentials_provider_new_cached`.

## 5. Request flow

### 5.1 Per request

1. `ostree-repo-pull.c` queues a fetch exactly as today.
2. `initiate_next_curl_request` (`ostree-fetcher-curl.c:812`) inspects the
   base URI scheme. For `s3`:
   1. Parse the location (cached on the fetcher per base URI).
   2. If the fetcher has no region yet for this location, resolve it.
   3. If the fetcher holds credentials for this `(profile, region)` with
      more than 300 seconds of validity left, continue. Otherwise park the
      request on a waiting list; if no credential fetch is in flight for
      that key, start one. On completion, all parked requests resume at
      step 2.3, or all fail with the credential error.
   4. Build the request URL from the location plus the request's subpath.
   5. Sign; append the returned headers to `req->req_headers`.
   6. Disable `CURLOPT_FOLLOWLOCATION` for this request.
3. curl sends the request. Mirror failover and retries re-enter step 2, so
   each attempt is signed with a fresh timestamp.

Signed headers: `host`, `x-amz-date`, `x-amz-content-sha256`
(`UNSIGNED-PAYLOAD`), and `x-amz-security-token` when present. Conditional
headers (`If-None-Match`, `If-Modified-Since`) and user `http-headers` are
sent but not signed.

The assert at `ostree-fetcher-curl.c:341` (effective URL starts with `http`
or `file:`) continues to hold because curl only ever sees the rewritten URL.

### 5.2 Credential lifetime

- Refresh threshold: 300 seconds before expiry.
- On a 403 whose S3 error code is `ExpiredToken`, `TokenRefreshRequired` or
  `RequestExpired`: drop the fetcher's credentials for that key, fetch with
  `force_refresh = TRUE`, and retry the request once. A second failure is
  reported unchanged.

### 5.3 SSO limitation

`aws-c-auth` reads the access token cached by `aws sso login` but does not
refresh it (`token_provider_sso_session.c` has a TODO). When the token is
expired the pull fails with a message naming both workarounds:

- run `aws sso login --profile <p>` (or any AWS CLI command that refreshes
  the cache), or
- configure the profile with
  `credential_process = aws configure export-credentials --profile <p> --format process`.

## 6. Errors

| Situation | Result |
|---|---|
| No provider yields credentials | `G_IO_ERROR_PERMISSION_DENIED`, message lists the sources tried |
| SSO token expired | `G_IO_ERROR_PERMISSION_DENIED`, message from 5.3 |
| No region resolvable | `G_IO_ERROR_INVALID_ARGUMENT`, message names `?region=` and `AWS_REGION` |
| Invalid `s3://` URL | `G_IO_ERROR_INVALID_ARGUMENT` at URI validation |
| 403 | `G_IO_ERROR_PERMISSION_DENIED`, message includes S3 `<Code>` and `<Message>` |
| 404 | `G_IO_ERROR_NOT_FOUND`, same as HTTP remotes |
| 301 / 307 | `G_IO_ERROR_FAILED`: "bucket is in region X", X from `x-amz-bucket-region` or the error body |
| 5xx, 503 `SlowDown` | existing retry logic, unchanged |

For non-2xx `s3://` responses the body (capped at 64 KiB) is retained in
memory for error parsing instead of being written to the output.

## 7. Performance

S3 serves HTTP/1.1 only, so the curl backend's HTTP/2 multiplexing does not
apply and connection count is the throughput ceiling.

- **Inline signing.** Credentials are fetched off-thread once per
  `(profile, region)`; signing runs on the fetcher's thread with no hop.
- **Process-wide cache.** ostree creates a fetcher at three call sites
  (`ostree-repo-pull.c:3238`, `:5741`, `:6400`); all share one resolution.
- **Lazy start.** No AWS runtime, thread or TLS context unless an `s3://`
  URL is used.
- **Connection cap.** `CURLMOPT_MAX_TOTAL_CONNECTIONS` is hard-coded to 8
  (`ostree-fetcher-curl.c:255`). It becomes
  `MAX (8, opt_max_outstanding_fetcher_requests)`, updated in
  `_ostree_fetcher_set_max_outstanding_fetcher_requests`.
- **Concurrency default.** When a remote's URL is `s3://` and the user has
  not set `max-outstanding-fetcher-requests`, the default is 32 instead of
  8. The value is confirmed or replaced by the benchmark.
- **Connection reuse.** Measured early against real S3 (connections opened
  versus requests made). If curl closes connections as described in
  upstream issue #3350, fixing that for `s3://` is part of this work.

### Acceptance benchmark

Pull a fixed archive repo of roughly 5,000 objects from a bucket in the
same region as the client, three ways: plain unsigned HTTPS to a
throwaway public-read copy of the repo (a test-only bucket holding no real
content), `s3://` at concurrency 8, `s3://` at the new default. Record
wall time, objects per second, and connections opened.

Pass criteria:

- `s3://` at concurrency 8 is within 5% of plain HTTPS wall time.
- Connections opened stays at or below the concurrency limit plus retries
  (no per-request reconnects).
- `s3://` at the new default is faster than at 8.

## 8. Build integration

- `configure.ac`: `--with-aws` (default `no`). Requires `--with-curl`;
  configure errors otherwise.
- Detection: `AC_CHECK_HEADER([aws/auth/credentials.h])` and a link check
  for `aws_credentials_provider_new_sso`, since `aws-c-auth` ships no
  pkg-config files. `AWS_CFLAGS` and `AWS_LIBS` are precious variables;
  default `AWS_LIBS` is
  `-laws-c-auth -laws-c-http -laws-c-io -laws-c-cal -laws-c-sdkutils -laws-c-common`.
- Minimum `aws-c-auth`: 0.9.1 (verified to contain the SSO provider; the
  version in nixpkgs). Enforced by the link check for the SSO symbol.
- Defines `HAVE_AWS`; automake conditional `USE_AWS`.
- `Makefile-libostree.am`: new sources added inside the existing
  `if USE_CURL` block under a nested `if USE_AWS`.
- `OSTREE_FEATURES` gains `aws`, visible in `ostree --version`.
- No change to the minimum curl version.

## 9. Dev environment

A `flake.nix` at the repository root provides a dev shell with ostree's
build dependencies, curl, the AWS C libraries and a local S3 server. It is
fork tooling, kept in its own commit separate from the libostree changes.

## 10. Testing

### 10.1 Unit tests — `tests/test-fetcher-s3.c`

GLib test program, built only with `USE_AWS`. No network.

- Location parsing: bucket, prefix, each query parameter; rejection of
  unknown parameters, fragments, userinfo, empty bucket.
- Request URL construction: virtual-hosted; dotted bucket; endpoint
  override with and without port; `us-gov-west-1`.
- Key encoding, including `+`, spaces and non-ASCII.
- Error body parsing: `AccessDenied`, `ExpiredToken`, `PermanentRedirect`
  with region, non-XML body.
- Signing: a fixed credential, timestamp and URL produce the expected
  `Authorization` header. To make the timestamp fixable,
  `_ostree_aws_sign_get` has an internal variant taking an explicit time,
  exposed only to the test.

### 10.2 Integration test — `tests/test-pull-s3.sh`

Modelled on the existing `test-pull-*.sh` scripts. Skips when the S3 server
binary is missing or the build lacks the `aws` feature.

Starts a local S3-compatible server, creates a bucket, uploads an archive
repo, then asserts:

1. Pull a ref via `s3://bucket/repo?endpoint=…&region=us-east-1` with static
   environment credentials.
2. Pull with temporary credentials carrying a session token.
3. Pull with static deltas required (`--require-static-deltas`), covering
   `+` in delta paths.
4. Wrong secret key: pull fails, message contains
   `SignatureDoesNotMatch`.
5. Missing ref: pull fails with not-found.
6. Unknown query parameter: `remote add` or pull fails at validation.

Server: MinIO, because it verifies SigV4 and issues temporary credentials
via STS. If MinIO is unavailable in the dev shell, substitute another
server that does both and record the substitution in the test header.

### 10.3 Existing suite

`make check` passes with and without `--with-aws`, confirming `http(s)://`
and `file://` behaviour is unchanged.

### 10.4 Manual checklist — `docs/s3-manual-test.md` plus `tests/manual/s3-check.sh`

Run by a person with real AWS access, once in commercial and once in
GovCloud:

- EC2 instance role; EKS IRSA; ECS task role; SSO profile; assume-role
  profile; `credential_process` profile; static environment keys.
- Bucket in a different region than resolved: region error names the
  correct region.
- Expired SSO token: message names both workarounds.
- A pull that outlives its credentials continues after refresh.
- The acceptance benchmark from section 7.

## 11. Assumptions to verify first

The implementation plan starts by checking these; if either is false the
affected section is revised before further work.

1. `aws_sign_request_aws` completes synchronously on the calling thread
   when given credentials directly rather than a provider (section 5.1).
   Fallback if false: sign via the async path and park the request, as for
   credential fetches.
2. `aws_credentials_provider_new_sso` returns NULL, rather than a provider
   that fails later, when the profile has no SSO configuration (section
   4.4). Fallback if false: inspect the profile ourselves before
   constructing it.

## 12. Delivery

A branch based on `fdef7e6`, as separate commits so it rebases onto future
upstream:

1. configure / automake integration (`--with-aws`, feature string)
2. `ostree-fetcher-s3` unit and its unit tests
3. `ostree-aws` unit and signing test
4. curl backend wiring
5. URI validation and pull-side edits
6. integration test
7. manual checklist and benchmark script
8. `flake.nix`

## 13. Amendments from planning

Found while writing the implementation plan
(`docs/superpowers/plans/2026-10-09-s3-fetcher.md`). Where these differ
from sections 1-12, this section wins.

### Verified

- Both assumptions in section 11 hold against aws-c-auth 0.9.1: signing
  with explicit credentials completes synchronously on the calling thread,
  and the SSO provider constructor returns NULL for a profile without SSO
  settings. The fallbacks are not needed.

### Corrections

- **Expired tokens arrive as HTTP 400.** Real S3 returns `ExpiredToken`
  with status 400, not 403. The retry-once rule in 5.2 keys on the S3
  error code (`ExpiredToken`, `TokenRefreshRequired`, `RequestExpired`)
  whatever the status.
- **Wrong region is not always a redirect.** With SigV4, S3 can answer
  400 `AuthorizationHeaderMalformed` carrying the right `<Region>`. That
  is reported the same way as a 301/307 in section 6.
- **SSO-expired cannot be detected from the chain's error.** The chain
  reports only its last provider's failure. The message from 5.3 is
  therefore appended whenever credential resolution fails and the profile
  is SSO-configured.
- **Upstream already treats HTTP 403 as not-found for `http(s)://`
  remotes** (`ostree-fetcher-util.c`). The strict 403 policy in section 2
  makes `s3://` deliberately stricter than existing remotes; it is not
  matching existing behaviour.
- **`max-outstanding-fetcher-requests` is a no-op upstream.** The pull
  code reads it with GVariant type `b` while the CLI stores `u`. The fix
  (one character, two sites) is part of this work because section 7's
  default and benchmark depend on the option working. It also makes the
  option take effect for `http(s)://` remotes.

### Interface changes to sections 4.2 and 4.3

- `_ostree_s3_location_to_request_url` takes a third argument, the
  request's subpath.
- Added `_ostree_s3_credentials_usable (expiry, fetched_at, now)`: the
  refresh rule from 5.2 as a pure function, plus a guard so credentials
  that arrive already inside the 300-second window are used for up to 60
  seconds instead of being refetched in a loop.
- Added `_ostree_aws_credentials_new_static` and
  `_ostree_aws_sign_get_at` (explicit timestamp) as test seams.
- A forced credential refresh replaces the cached provider and leaks the
  old one, since another fetcher may have a query in flight on it.

### Gaps filled

- Buckets that are not a valid DNS label (uppercase, underscore, shorter
  than 3 or longer than 63 characters) use path-style addressing, like
  dotted buckets in section 3.
- An `?endpoint=` remote with no resolvable region signs with
  `us-east-1`.
- A port in the `s3://` authority is rejected.

### Environment

- nixpkgs marks MinIO insecure (unmaintained). The dev-shell flake permits
  that single package; it is run only on localhost by the integration
  test.
- The dev shell is the first commit, not the last as listed in section
  12, because nothing builds without it.

## 14. Performance tuning (2026-10-13)

Benchmark-driven changes made after the first real-world report. These
replace the numbers in section 7.

### What was measured

From a workstation to the GovCloud bucket: about 40 ms round trip, 50-60 ms
per small GET on a warm connection, about 1.5 MiB/s per connection, and a
total that scales with connections up to roughly 22 MiB/s. That link's
capacity swung between 5 and 20 MiB/s from minute to minute, so tuning was
done against a local throttling server reproducing those four numbers and
serving a mirror of real refs, then confirmed against S3.

### Changes

| Change | Why |
|---|---|
| Default concurrent requests for `s3://`: 32 -> 128 | Each object costs a round trip; an 88k-object pull is latency-bound. 156 s at 32, 88 s at 64, 61 s at 128, flat at 256. |
| Delta parts downloading at once for `s3://`: 2 -> 16 | With two connections at 1.5 MiB/s a 642 MiB delta used a seventh of the link. 136 s at 2, 51 s at 8, 45 s at 16. At most two parts are decompressed and applied at a time (see "Repo safety"); downloaded parts wait on disk, and downloading plus waiting parts never exceed 16. |
| Objects over 4 MiB are fetched as parallel 4 MiB ranges | Without it the end of a pull waits on its largest object arriving alone on one slow connection. 32 s -> 22 s for a 434 MiB pull whose largest object is 26 MiB; the link floor is 20 s. |

Ranged fetching, in detail:

- Every eligible request (a download to a file, with no conditional
  headers) asks for `Range: bytes=0-4194303`. A 206 response carries the
  total size in `Content-Range`; if there is more, the remainder is fetched
  as further ranged requests that `pwrite` into the same tmpfile.
- Follow-up ranges send `If-Match` with the first response's ETag, so parts
  of two different objects are never combined.
- A 416 (a zero-length object) is retried once without a range.
- Follow-up ranges are extra transfers beyond the pull's request limit,
  bounded only by the fetcher's connection cap. With
  `--max-outstanding-fetcher-requests=1`, a large object still uses several
  connections.
- In-memory requests (summary, config, refs) and conditional requests are
  never split.

### Tried and rejected

- **Raising the write limit (3).** No effect on small-object pulls and
  about 10% on delta pulls. Left at upstream's value, which exists to
  protect other I/O on the device.
- **More than 128 concurrent requests.** No further gain.
- **Dropping the signing layer's overhead.** `s3://` is about 8% slower
  than plain HTTP at 8,000 requests per second on a loopback link, which
  is nothing at real latencies.

### Still on the table

- Delta pulls finish about 15 s above the link floor because applying parts
  is CPU-bound and partly serial behind the write limit.
- On a fast link the pull is bound by ostree's main thread, not the
  network.
- SSO token refresh (5.3) is no longer hypothetical: the cached token
  expired during this work and pulls failed until credentials were supplied
  another way.

### Repo safety (audited 2026-10-13)

The question asked: can the performance work leave the repo on disk in a
state stock ostree could not, or make a corrupt object more likely?

**Where the fetcher's output is trusted.** On a normal pull every object is
checked before it enters the repo: content is re-parsed and rewritten
against its expected checksum, metadata checksums are compared, and delta
parts are checked against the superblock. A fetcher bug there can fail a
pull but cannot store a wrong object. The exception is `--http-trusted`
mirroring into an archive repo, which links the fetched file into
`objects/` unverified. Stock ostree trusts one HTTP response there; ranged
fetching assembles the file from several, so it must be no weaker:

- each follow-up range must be a 206 of exactly the requested length;
- follow-up ranges are started only when the first response has a strong
  ETag, and each carries it as `If-Match`; otherwise the object is fetched
  whole, so two versions of an object can never be combined;
- the assembled file's size is checked against the advertised total;
- the parent request completes exactly once, and only after every range
  has finished; any failure fails the whole object, and the tmpfile, never
  handed over, is discarded.

A server that sends wrong bytes with a correct length defeats
`--http-trusted` with or without ranges, as before.

**Partial commits.** The `.commitpartial` code is unchanged. It is written
when a commit arrives before its contents and removed only when a pull
completes successfully, which relies on each fetch completing exactly once
and succeeding only with complete bytes. `tests/test-pull-s3-faults.sh`
checks, for failed, truncated, ignored and corrupt ranges and for an
object changing mid-download, that the pull fails, no partial object is
left, the commit stays marked partial, `fsck` passes, and the next pull
recovers and clears the marker. It also covers trusted mirroring.

**A regression found and fixed.** Allowing 16 delta parts to download at
once also let up to 16 be decompressed in memory and applied at once,
because each part was applied as soon as it arrived: a 642 MiB delta
peaked at 545 MiB instead of 90-130 MiB. Downloaded parts now queue on
disk and at most two are applied at a time, the bound the old download
limit gave. Peak memory measured against the simulator, old build versus
new: delta 92 -> 151 MiB, software ref 33 -> 38 MiB, sysroot 82 -> 63 MiB.

**Disk use while pulling.** Higher concurrency means more temporary files
at once: up to 128 objects downloading and up to 16 delta parts downloaded
or waiting. These are anonymous tmpfiles that vanish on failure or crash,
but `min-free-space` is enforced when objects are written, not while they
download, so a nearly full disk can run out mid-pull sooner than before.
The pull fails cleanly when it does.
