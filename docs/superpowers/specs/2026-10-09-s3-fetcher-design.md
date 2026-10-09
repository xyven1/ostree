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
