# Native `s3://` Pull Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `ostree pull` works against a private S3 bucket through an `s3://bucket/prefix` remote URL, using the host's existing AWS credentials.

**Architecture:** Two new units sit beside the curl fetcher backend. `ostree-fetcher-s3` is pure GLib logic (URL parsing, request-URL building, S3 error parsing). `ostree-aws` owns a lazily started, process-wide `aws-c-auth` runtime that resolves credentials and region and produces SigV4 headers. `ostree-fetcher-curl.c` calls both: it rewrites each `s3://` request to HTTPS, attaches the signed headers, and lets curl transport it.

**Tech Stack:** C (GLib/GIO), libcurl, aws-c-auth 0.9.1 (+ aws-c-http, aws-c-io, aws-c-cal, aws-c-sdkutils, aws-c-common), autotools, Nix dev shell, MinIO for integration tests.

**Spec:** `docs/superpowers/specs/2026-10-09-s3-fetcher-design.md` (read section 13, "Amendments from planning", first: it records where this plan deliberately differs from sections 1-12).

## Global Constraints

- Base commit: upstream ostree `fdef7e6`. Work on branch `worktree-s3-fetcher`.
- Pull only. Curl backend only. No new remote config keys.
- All S3 code is behind `#ifdef HAVE_AWS` / `if USE_AWS`. A build without `--with-aws` must compile and behave exactly as upstream, except for the one-character `max-outstanding-fetcher-requests` type fix in Task 8.
- `--with-aws` requires `--with-curl`; configure must error otherwise.
- Minimum aws-c-auth: 0.9.1, enforced by a link check for `aws_credentials_provider_new_sso`.
- Allowed URL query parameters: exactly `region`, `profile`, `endpoint`. Anything else is an error.
- Region order: `?region=` → `AWS_REGION` → `AWS_DEFAULT_REGION` → profile `region` → IMDS → error.
- Credential chain order: environment → SSO → profile → web identity → ECS, else IMDS.
- HTTP 403 from S3 is `G_IO_ERROR_PERMISSION_DENIED`, never not-found. HTTP 404 is `G_IO_ERROR_NOT_FOUND`.
- Redirects are never followed for `s3://` requests.
- Credentials refresh 300 seconds before expiry. S3 error bodies are capped at 64 KiB.
- Default `max-outstanding-fetcher-requests` for `s3://` remotes: 32. Curl connection cap: `MAX (8, max-outstanding-fetcher-requests)`.
- Every new source file carries the LGPL-2.0+ header used throughout `src/libostree/`.
- Code style: match the surrounding file (GNU style, 2-space indent, `g_autoptr`/`g_autofree`). Run `clang-format -i` on new files only if `clang-format` is in the dev shell; never reformat existing files.
- Every build and test command runs inside the dev shell: prefix with `nix develop -c`.
- Commit after every task. End every commit message with the line
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do not push: the only remote is upstream `ostreedev/ostree`.

## Verified During Planning

These were checked against aws-c-auth 0.9.1, aws-c-io 0.27.2, aws-c-http 0.11.0, aws-c-common 0.12.4, aws-c-sdkutils 0.2.4 and GLib 2.88.3 from nixpkgs. Task 1 re-runs the first two inside the dev shell.

- **Spec assumption 1 holds.** `aws_sign_request_aws` invokes its callback before returning, on the calling thread, when given explicit credentials.
- **Spec assumption 2 holds.** `aws_credentials_provider_new_sso` returns NULL (`AWS_AUTH_CREDENTIALS_PROVIDER_SSO_SOURCE_FAILURE`) when the profile has no SSO configuration.
- The environment provider can invoke the credentials callback synchronously on the calling thread; other providers call back on the AWS event-loop thread. `ostree-aws.c` routes both through a `GTask`.
- The exact contents of `ostree-fetcher-s3.{c,h}`, `ostree-aws.{c,h}` and `tests/test-fetcher-s3.c` given in Tasks 3 and 4 were compiled with `-Wall -Wmissing-prototypes` and all 13 tests passed. Type them in as written.
- The two expected `Authorization` values in the signing test were produced by an independent SigV4 implementation (Python `hashlib`/`hmac`) and match aws-c-auth's output.
- Upstream bug: `ostree-repo-pull.c` looks up `max-outstanding-fetcher-requests` with GVariant type `"b"` while the CLI stores `"u"`, so the option is silently ignored. Task 8 fixes it; the benchmark depends on it.
- Upstream already maps HTTP 403 to not-found for `http(s)://` remotes (`ostree-fetcher-util.c:231`). `s3://` is deliberately stricter.
- Real S3 returns `ExpiredToken` with HTTP **400**, not 403.
- nixpkgs marks `minio` insecure (unmaintained). The dev shell permits that one package; it is only ever run on localhost by the test.
- The git submodules `libglnx` and `bsdiff` are not initialized in this checkout.

Not verified, because they need the full build or real AWS: everything in Task 7 (curl wiring), the Nix dev shell contents, the MinIO test script, IMDS, SSO, IRSA and ECS.

## Review Focus

Inputs the spec implies but does not spell out, most likely first. Each has a pinning test in Task 3.

1. **Credentials that arrive already inside the 300-second refresh window** (short SSO or role sessions). Expected: used immediately, not refetched in a loop. Test: `/s3/credentials-usable`.
2. **Object names with `+`, space, `%`, `?`, `&` or non-ASCII bytes.** Expected: each encoded exactly once, so the signature matches what curl sends. Test: `/s3/url/key-encoding`.
3. **Legacy bucket names** (dots, underscores, under 3 characters). Expected: path-style URL, not a hostname that fails TLS or DNS. Test: `/s3/url/legacy-buckets`.
4. **Slash variations** (`s3://b`, `s3://b/`, `s3://b//repo//`, subpaths with a leading slash). Expected: one canonical URL. Test: `/s3/url/slashes`.
5. **A non-S3 error body** (proxy HTML page, truncated XML, binary junk). Expected: treated as "no S3 error code", no crash, out-parameters untouched. Test: `/s3/error-body/not-s3`.

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `flake.nix`, `flake.lock` | create | Dev shell: ostree build deps, curl, AWS C libraries, MinIO, `mc` |
| `tests/manual/s3-aws-probe.c` | create | Standalone check of the two aws-c-auth behaviours the design depends on |
| `configure.ac` | modify | `--with-aws`, `HAVE_AWS`, `USE_AWS`, `aws` feature string |
| `src/libostree/ostree-fetcher-s3.{c,h}` | create | Parse `s3://`, build request URL, parse S3 error XML, credential-freshness rule |
| `src/libostree/ostree-aws.{c,h}` | create | AWS runtime, credential chain + cache, region resolution, SigV4 signing |
| `Makefile-libostree.am` | modify | Compile the two units under `USE_CURL` + `USE_AWS` |
| `src/libostree/ostree-fetcher-uri.c` | modify | Accept and validate the `s3` scheme |
| `src/libostree/ostree-fetcher-curl.c` | modify | Per-request S3 flow, error mapping, connection cap |
| `src/libostree/ostree-repo-pull.c` | modify | S3 concurrency default, option type fix, status line |
| `tests/test-fetcher-s3.c` | create | Unit tests for both units and URI validation |
| `tests/test-pull-s3.sh` | create | End-to-end pulls against a local MinIO |
| `Makefile-tests.am` | modify | Register both tests under `USE_AWS` |
| `docs/s3-manual-test.md`, `tests/manual/s3-check.sh` | create | Real-AWS checklist and benchmark |

Commit order differs from spec section 12 in one way: the dev shell comes first, because nothing can be built without it.

---

### Task 1: Dev shell, baseline build, and aws-c-auth probe

**Files:**
- Create: `flake.nix`, `flake.lock` (generated)
- Create: `tests/manual/s3-aws-probe.c`

**Interfaces:**
- Consumes: nothing.
- Produces: a working `nix develop` shell; `/tmp/ostree-s3-baseline-check.log` listing which upstream tests pass and fail in this environment (later tasks compare against it).

- [ ] **Step 1: Write `flake.nix`**

```nix
{
  description = "Dev shell for the ostree s3:// fork";

  # Resolved through the local flake registry and pinned in flake.lock.
  inputs.nixpkgs.url = "nixpkgs";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs {
        inherit system;
        # nixpkgs marks MinIO insecure because it is unmaintained. It is used
        # here only as a localhost S3 fixture by tests/test-pull-s3.sh.
        config.allowInsecurePredicate = pkg: nixpkgs.lib.getName pkg == "minio";
      }));
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ pkgs.ostree ];
          packages = with pkgs; [
            autoconf
            automake
            libtool
            pkg-config
            bison
            curl
            aws-c-auth
            aws-c-http
            aws-c-io
            aws-c-cal
            aws-c-sdkutils
            aws-c-common
            minio
            minio-client
            python3
          ];
        };
      });
    };
}
```

- [ ] **Step 2: Enter the shell and confirm the tools exist**

Nix only sees files git knows about, so stage the flake first.

Run:
```bash
git add flake.nix
nix develop -c bash -c 'curl --version | head -1; minio --version | head -1; mc --version | head -1; echo shell-ok'
```
Expected: a curl 8.x version line, a MinIO version line, an `mc` version line, then `shell-ok`. `flake.lock` now exists.

If `nix develop` fails because an attribute is missing (for example `minio-client`), fix the package name in `flake.nix` and re-run; do not drop the package.

- [ ] **Step 3: Initialize submodules and build upstream unchanged**

Run:
```bash
git submodule update --init
nix develop -c bash -c 'env NOCONFIGURE=1 ./autogen.sh && ./configure --with-curl && make -j"$(nproc)"'
nix develop -c ./ostree --version
```
Expected: the build succeeds and the feature list printed by `ostree --version` contains `- libcurl` and does not contain `- aws`.

If configure reports a missing dependency, add that package to `packages` in `flake.nix` and re-run.

- [ ] **Step 4: Record the baseline test results**

Some upstream tests need privileges or kernel features this machine may lack. Record what passes now so later tasks can tell their own regressions from pre-existing failures.

Run:
```bash
nix develop -c bash -c 'make check -j"$(nproc)" -k > /tmp/ostree-s3-baseline-check.log 2>&1; grep -E "^# (TOTAL|PASS|SKIP|FAIL|ERROR):" /tmp/ostree-s3-baseline-check.log; grep -E "^(FAIL|ERROR):" /tmp/ostree-s3-baseline-check.log | sort > /tmp/ostree-s3-baseline-failures.txt; wc -l < /tmp/ostree-s3-baseline-failures.txt'
```
Expected: a summary block and a count of failing tests (possibly 0). Whatever the count, this is the baseline; do not try to fix upstream failures.

- [ ] **Step 5: Write the probe `tests/manual/s3-aws-probe.c`**

```c
/*
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Standalone check of the two aws-c-auth behaviours the s3:// fetcher
 * design depends on. Not built by the normal build; see
 * docs/superpowers/specs/2026-10-09-s3-fetcher-design.md section 11.
 *
 *   gcc -o probe s3-aws-probe.c -laws-c-auth -laws-c-http -laws-c-io -laws-c-common
 *
 * Run with AWS_CONFIG_FILE pointing at a config whose [default] profile
 * has no SSO settings. Exits 0 only if both behaviours hold.
 */
#include <aws/auth/auth.h>
#include <aws/auth/credentials.h>
#include <aws/auth/signable.h>
#include <aws/auth/signing.h>
#include <aws/auth/signing_config.h>
#include <aws/auth/signing_result.h>
#include <aws/common/date_time.h>
#include <aws/common/string.h>
#include <aws/common/thread.h>
#include <aws/http/request_response.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/tls_channel_handler.h>
#include <stdio.h>

static int sign_cb_called = 0;
static aws_thread_id_t sign_cb_thread;

static void
on_sign (struct aws_signing_result *result, int error_code, void *user_data)
{
  sign_cb_called = 1;
  sign_cb_thread = aws_thread_current_thread_id ();
  if (error_code != 0)
    {
      printf ("sign error: %s\n", aws_error_debug_str (error_code));
      return;
    }
  struct aws_array_list *headers = NULL;
  aws_signing_result_get_property_list (result, g_aws_http_headers_property_list_name, &headers);
  const size_t n = headers ? aws_array_list_length (headers) : 0;
  for (size_t i = 0; i < n; i++)
    {
      struct aws_signing_result_property p;
      aws_array_list_get_at (headers, &p, i);
      printf ("HDR %s: %s\n", aws_string_c_str (p.name), aws_string_c_str (p.value));
    }
}

int
main (void)
{
  struct aws_allocator *a = aws_default_allocator ();
  aws_auth_library_init (a);

  /* Assumption 1: signing with explicit credentials completes inline. */
  struct aws_credentials *creds
      = aws_credentials_new (a, aws_byte_cursor_from_c_str ("AKIAIOSFODNN7EXAMPLE"),
                             aws_byte_cursor_from_c_str ("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"),
                             (struct aws_byte_cursor){ 0 }, UINT64_MAX);
  struct aws_http_message *msg = aws_http_message_new_request (a);
  aws_http_message_set_request_method (msg, aws_byte_cursor_from_c_str ("GET"));
  aws_http_message_set_request_path (
      msg, aws_byte_cursor_from_c_str ("/repo/deltas/ab/c%2Bd/superblock"));
  struct aws_http_header host = { .name = aws_byte_cursor_from_c_str ("host"),
                                  .value = aws_byte_cursor_from_c_str (
                                      "examplebucket.s3.amazonaws.com") };
  aws_http_message_add_header (msg, host);
  struct aws_signable *signable = aws_signable_new_http_request (a, msg);

  struct aws_signing_config_aws cfg;
  AWS_ZERO_STRUCT (cfg);
  cfg.config_type = AWS_SIGNING_CONFIG_AWS;
  cfg.algorithm = AWS_SIGNING_ALGORITHM_V4;
  cfg.signature_type = AWS_ST_HTTP_REQUEST_HEADERS;
  cfg.region = aws_byte_cursor_from_c_str ("us-east-1");
  cfg.service = aws_byte_cursor_from_c_str ("s3");
  aws_date_time_init_epoch_secs (&cfg.date, 1369353600.0); /* 20130524T000000Z */
  cfg.flags.use_double_uri_encode = false;
  cfg.flags.should_normalize_uri_path = false;
  cfg.signed_body_value = g_aws_signed_body_value_unsigned_payload;
  cfg.signed_body_header = AWS_SBHT_X_AMZ_CONTENT_SHA256;
  cfg.credentials = creds;

  const int rc
      = aws_sign_request_aws (a, signable, (struct aws_signing_config_base *)&cfg, on_sign, NULL);
  const int same_thread
      = sign_cb_called
        && aws_thread_thread_id_equal (sign_cb_thread, aws_thread_current_thread_id ());
  printf ("SIGN rc=%d callback_called_before_return=%d same_thread=%d\n", rc, sign_cb_called,
          same_thread);
  const int sign_ok = rc == 0 && sign_cb_called && same_thread;

  /* Assumption 2: no SSO provider when the profile has no SSO settings. */
  struct aws_event_loop_group_options elg_opts = { .loop_count = 1 };
  struct aws_event_loop_group *elg = aws_event_loop_group_new (a, &elg_opts);
  struct aws_host_resolver_default_options res_opts = { .max_entries = 8, .el_group = elg };
  struct aws_host_resolver *resolver = aws_host_resolver_new_default (a, &res_opts);
  struct aws_client_bootstrap_options bs_opts
      = { .event_loop_group = elg, .host_resolver = resolver };
  struct aws_client_bootstrap *bootstrap = aws_client_bootstrap_new (a, &bs_opts);
  struct aws_tls_ctx_options tls_opts;
  aws_tls_ctx_options_init_default_client (&tls_opts, a);
  struct aws_tls_ctx *tls = aws_tls_client_ctx_new (a, &tls_opts);
  aws_tls_ctx_options_clean_up (&tls_opts);

  struct aws_credentials_provider_sso_options sso_opts;
  AWS_ZERO_STRUCT (sso_opts);
  sso_opts.bootstrap = bootstrap;
  sso_opts.tls_ctx = tls;
  struct aws_credentials_provider *sso = aws_credentials_provider_new_sso (a, &sso_opts);
  printf ("SSO provider=%s\n", sso ? "non-NULL" : "NULL");

  return (sign_ok && sso == NULL) ? 0 : 1;
}
```

- [ ] **Step 6: Run the probe**

Run:
```bash
mkdir -p /tmp/s3-probe && printf '[default]\nregion = us-east-1\n' > /tmp/s3-probe/config
nix develop -c bash -c 'gcc -o /tmp/s3-probe/probe tests/manual/s3-aws-probe.c -laws-c-auth -laws-c-http -laws-c-io -laws-c-common && AWS_CONFIG_FILE=/tmp/s3-probe/config AWS_SHARED_CREDENTIALS_FILE=/tmp/s3-probe/none /tmp/s3-probe/probe'
```
Expected output, exactly these two result lines among the header lines, and exit status 0:
```
SIGN rc=0 callback_called_before_return=1 same_thread=1
SSO provider=NULL
```
and an `Authorization` header ending in
`Signature=1f1b4657c75f09877e88547ee17c26163a6c952895a7b120ef92e311633ed4e7`.

**If either result line differs, stop and report.** The fallbacks are in spec section 11 and change Tasks 4 and 7; do not improvise them.

- [ ] **Step 7: Commit**

```bash
git add flake.nix flake.lock tests/manual/s3-aws-probe.c
git commit -m "build: add Nix dev shell and aws-c-auth probe for s3:// work

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `--with-aws` configure option

**Files:**
- Modify: `configure.ac` (insert after line 164, the `libcurl` feature line)

**Interfaces:**
- Consumes: the dev shell from Task 1.
- Produces: C macro `HAVE_AWS` (in `config.h`), automake conditional `USE_AWS`, substituted variables `AWS_CFLAGS` and `AWS_LIBS`, feature string `aws` in `ostree --version`.

- [ ] **Step 1: Write the failing check**

Run:
```bash
nix develop -c bash -c './configure --with-curl --with-aws >/dev/null 2>&1; make -j"$(nproc)" >/dev/null && ./ostree --version | grep -- "- aws"'
```
Expected: FAIL (no output, non-zero exit). configure ignores the unknown option and the feature is absent.

- [ ] **Step 2: Add the option**

In `configure.ac`, find:

```m4
AM_CONDITIONAL(USE_CURL, test x$with_curl != xno)
if test x$with_curl = xyes; then OSTREE_FEATURES="$OSTREE_FEATURES libcurl"; fi
```

Insert immediately after it:

```m4

dnl Native s3:// remotes: credentials and SigV4 signing via aws-c-auth,
dnl transport via libcurl. aws-c-auth ships CMake config only, so there is
dnl no pkg-config check; override AWS_CFLAGS / AWS_LIBS if it is not on the
dnl default search path.
AC_ARG_WITH(aws,
	    AS_HELP_STRING([--with-aws], [Enable s3:// remotes using aws-c-auth; requires --with-curl @<:@default=no@:>@]),
	    [], [with_aws=no])
AC_ARG_VAR([AWS_CFLAGS], [C compiler flags for the AWS C libraries])
AC_ARG_VAR([AWS_LIBS], [linker flags for the AWS C libraries])
AS_IF([test x$with_aws != xno], [
  AS_IF([test x$with_curl != xyes], [
    AC_MSG_ERROR([--with-aws requires --with-curl])
  ])
  AS_IF([test -z "$AWS_LIBS"], [
    AWS_LIBS="-laws-c-auth -laws-c-http -laws-c-io -laws-c-cal -laws-c-sdkutils -laws-c-common"
  ])
  save_CPPFLAGS=$CPPFLAGS
  save_LIBS=$LIBS
  CPPFLAGS="$CPPFLAGS $AWS_CFLAGS"
  LIBS="$AWS_LIBS $LIBS"
  AC_CHECK_HEADER([aws/auth/credentials.h], [],
    [AC_MSG_ERROR([aws-c-auth headers not found; set AWS_CFLAGS])])
  dnl The SSO provider is the newest API we use; its presence is the version floor.
  AC_CHECK_FUNC([aws_credentials_provider_new_sso], [],
    [AC_MSG_ERROR([aws-c-auth >= 0.9.1 not found; set AWS_LIBS])])
  CPPFLAGS=$save_CPPFLAGS
  LIBS=$save_LIBS
  with_aws=yes
  AC_DEFINE([HAVE_AWS], 1, [Define if s3:// remotes are supported via aws-c-auth])
  OSTREE_FEATURES="$OSTREE_FEATURES aws"
])
AM_CONDITIONAL(USE_AWS, test x$with_aws = xyes)
```

- [ ] **Step 3: Run the check again**

Run:
```bash
nix develop -c bash -c 'env NOCONFIGURE=1 ./autogen.sh >/dev/null && ./configure --with-curl --with-aws >/dev/null && make -j"$(nproc)" >/dev/null && ./ostree --version | grep -- "- aws"'
```
Expected: PASS, prints ` - aws`.

- [ ] **Step 4: Check the two failure modes**

Run:
```bash
rm -rf /tmp/s3-conf && mkdir /tmp/s3-conf && cd /tmp/s3-conf
nix develop "$OLDPWD" -c bash -c "$OLDPWD/configure --without-curl --with-aws 2>&1 | tail -1"
nix develop "$OLDPWD" -c bash -c "$OLDPWD/configure --with-curl --with-aws AWS_LIBS=-lnonexistent-aws 2>&1 | tail -1"
cd "$OLDPWD"
```
Expected, in order:
```
configure: error: --with-aws requires --with-curl
configure: error: aws-c-auth >= 0.9.1 not found; set AWS_LIBS
```

- [ ] **Step 5: Commit**

```bash
git add configure.ac
git commit -m "build: add --with-aws option for s3:// remotes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: `ostree-fetcher-s3` unit

**Files:**
- Create: `src/libostree/ostree-fetcher-s3.h`
- Create: `src/libostree/ostree-fetcher-s3.c`
- Create: `tests/test-fetcher-s3.c`
- Modify: `Makefile-libostree.am` (the `if USE_CURL` block, around line 233)
- Modify: `Makefile-tests.am` (after the `if USE_GPGME … endif` block that follows `_installed_or_uninstalled_test_programs =`, around line 300)

**Interfaces:**
- Consumes: `USE_AWS`, `AWS_CFLAGS`, `AWS_LIBS` from Task 2.
- Produces (all in `ostree-fetcher-s3.h`):
  - `typedef struct { char *bucket; char *key_prefix; char *region; char *profile; char *endpoint; } OstreeS3Location;` with `g_autoptr` support
  - `OstreeS3Location *_ostree_s3_location_parse (GUri *uri, GError **error);`
  - `void _ostree_s3_location_free (OstreeS3Location *loc);`
  - `char *_ostree_s3_location_to_request_url (const OstreeS3Location *loc, const char *region, const char *subpath);`
  - `gboolean _ostree_s3_parse_error_body (const char *body, gsize len, char **out_code, char **out_message, char **out_region);`
  - `gboolean _ostree_s3_credentials_usable (guint64 expiry, guint64 fetched_at, guint64 now);`
  - `#define OSTREE_S3_CREDS_REFRESH_WINDOW_SECS 300`
  - `#define OSTREE_S3_CREDS_MIN_REFETCH_INTERVAL_SECS 60`

- [ ] **Step 1: Write the header `src/libostree/ostree-fetcher-s3.h`**

The tests cannot compile without the declarations, so the header comes first.

```c
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

/* Refresh credentials once they are this close to expiring. */
#define OSTREE_S3_CREDS_REFRESH_WINDOW_SECS 300
/* Never refetch credentials more often than this, even if they are close to expiring. */
#define OSTREE_S3_CREDS_MIN_REFETCH_INTERVAL_SECS 60

typedef struct
{
  char *bucket;
  char *key_prefix; /* decoded, no leading or trailing slash; may be "" */
  char *region;     /* from ?region=, or NULL */
  char *profile;    /* from ?profile=, or NULL */
  char *endpoint;   /* from ?endpoint=, normalized to scheme://host[:port], or NULL */
} OstreeS3Location;

void _ostree_s3_location_free (OstreeS3Location *loc);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeS3Location, _ostree_s3_location_free)

/* Parse and validate an s3:// URI. */
OstreeS3Location *_ostree_s3_location_parse (GUri *uri, GError **error);

/* Build the URL to request for @subpath (may be NULL) below the location.
 * @region must be non-NULL unless loc->endpoint is set. */
char *_ostree_s3_location_to_request_url (const OstreeS3Location *loc, const char *region,
                                          const char *subpath);

/* Extract fields from an S3 XML error body. Any out param may come back
 * NULL when the element is absent. Returns FALSE if @body is not an S3
 * error document. */
gboolean _ostree_s3_parse_error_body (const char *body, gsize len, char **out_code,
                                      char **out_message, char **out_region);

/* Whether credentials expiring at @expiry (seconds since the epoch,
 * G_MAXUINT64 for never) and obtained at @fetched_at may be used at @now. */
gboolean _ostree_s3_credentials_usable (guint64 expiry, guint64 fetched_at, guint64 now);

G_END_DECLS
```

- [ ] **Step 2: Write the failing tests `tests/test-fetcher-s3.c`**

```c
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

#include "ostree-fetcher-s3.h"
#include <gio/gio.h>
#include <glib.h>
#include <string.h>

/* The same flags _ostree_fetcher_uri_parse() uses. */
static OstreeS3Location *
parse (const char *str, GError **error)
{
  g_autoptr (GUri) uri = g_uri_parse (
      str, G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_ENCODED | G_URI_FLAGS_SCHEME_NORMALIZE, error);
  if (uri == NULL)
    return NULL;
  return _ostree_s3_location_parse (uri, error);
}

static void
assert_url (const char *s3, const char *region, const char *subpath, const char *expected)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeS3Location) loc = parse (s3, &error);
  g_assert_no_error (error);
  g_autofree char *url = _ostree_s3_location_to_request_url (loc, region, subpath);
  g_assert_cmpstr (url, ==, expected);
}

static void
assert_rejected (const char *s3, const char *needle)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeS3Location) loc = parse (s3, &error);
  g_assert_null (loc);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  if (strstr (error->message, needle) == NULL)
    g_error ("expected '%s' in error for %s, got: %s", needle, s3, error->message);
}

static void
test_parse_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeS3Location) loc
      = parse ("s3://my-bucket/some/repo?region=us-gov-west-1&profile=dev&endpoint=https://h:9000/",
               &error);
  g_assert_no_error (error);
  g_assert_cmpstr (loc->bucket, ==, "my-bucket");
  g_assert_cmpstr (loc->key_prefix, ==, "some/repo");
  g_assert_cmpstr (loc->region, ==, "us-gov-west-1");
  g_assert_cmpstr (loc->profile, ==, "dev");
  g_assert_cmpstr (loc->endpoint, ==, "https://h:9000");

  g_autoptr (OstreeS3Location) bare = parse ("s3://my-bucket", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (bare->key_prefix, ==, "");
  g_assert_null (bare->region);
  g_assert_null (bare->profile);
  g_assert_null (bare->endpoint);
}

static void
test_parse_rejects (void)
{
  assert_rejected ("s3://my-bucket/repo?regoin=us-east-1", "Unknown parameter 'regoin'");
  assert_rejected ("s3://my-bucket/repo?region=us-east-1&region=us-west-2", "more than once");
  assert_rejected ("s3://my-bucket/repo?region=", "is empty");
  assert_rejected ("s3://my-bucket/repo?region=US_EAST", "region may only contain");
  assert_rejected ("s3://my-bucket/repo?region=a.evil.example", "region may only contain");
  assert_rejected ("s3://my-bucket/repo#frag", "fragment");
  assert_rejected ("s3://key:secret@my-bucket/repo", "credentials are not allowed");
  assert_rejected ("s3://my-bucket:9000/repo", "port is not allowed");
  assert_rejected ("s3:///repo", "bucket");
  assert_rejected ("s3://my-bucket/repo?endpoint=ftp://h", "endpoint must look like");
  assert_rejected ("s3://my-bucket/repo?endpoint=http://h/sub/path", "endpoint must look like");
  assert_rejected ("s3://my-bucket/repo?endpoint=http://u@h", "endpoint must look like");
}

static void
test_url_addressing (void)
{
  assert_url ("s3://my-bucket/repo", "us-west-2", "objects/ab/cd.filez",
              "https://my-bucket.s3.us-west-2.amazonaws.com/repo/objects/ab/cd.filez");
  /* GovCloud uses the same hostname form */
  assert_url ("s3://my-bucket/repo", "us-gov-west-1", "config",
              "https://my-bucket.s3.us-gov-west-1.amazonaws.com/repo/config");
  /* Endpoint override is always path-style, whatever the bucket looks like */
  assert_url ("s3://b1/repo?endpoint=http://127.0.0.1:9000&region=us-east-1", "us-east-1", "summary",
              "http://127.0.0.1:9000/b1/repo/summary");
  assert_url ("s3://my-bucket/repo?endpoint=https://vpce.example.com", NULL, "summary",
              "https://vpce.example.com/my-bucket/repo/summary");
}

/* Review focus: legacy bucket names must not produce a broken hostname. */
static void
test_url_legacy_buckets (void)
{
  assert_url ("s3://my.dotted.bucket/repo", "us-east-1", "config",
              "https://s3.us-east-1.amazonaws.com/my.dotted.bucket/repo/config");
  assert_url ("s3://my_bucket/repo", "us-east-1", "config",
              "https://s3.us-east-1.amazonaws.com/my_bucket/repo/config");
  assert_url ("s3://ab/repo", "us-east-1", "config", "https://s3.us-east-1.amazonaws.com/ab/repo/config");
}

/* Review focus: every key character is encoded exactly once. */
static void
test_url_key_encoding (void)
{
  const char *base = "https://my-bucket.s3.us-east-1.amazonaws.com/repo/";
  struct
  {
    const char *subpath;
    const char *encoded;
  } cases[] = {
    { "deltas/ab/c+d/superblock", "deltas/ab/c%2Bd/superblock" },
    { "a b", "a%20b" },
    { "100%.txt", "100%25.txt" },
    { "tilde~dash-dot.under_", "tilde~dash-dot.under_" },
    { "caf\xc3\xa9", "caf%C3%A9" },
    { "q?x=1&y", "q%3Fx%3D1%26y" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *expected = g_strconcat (base, cases[i].encoded, NULL);
      assert_url ("s3://my-bucket/repo", "us-east-1", cases[i].subpath, expected);
    }
  /* An encoded prefix in the s3:// URL is decoded, then encoded once */
  assert_url ("s3://my-bucket/my%20repo/100%25", "us-east-1", "config",
              "https://my-bucket.s3.us-east-1.amazonaws.com/my%20repo/100%25/config");
}

/* Review focus: slash variations all mean the same location. */
static void
test_url_slashes (void)
{
  const char *expected = "https://my-bucket.s3.us-east-1.amazonaws.com/repo/config";
  assert_url ("s3://my-bucket/repo", "us-east-1", "config", expected);
  assert_url ("s3://my-bucket/repo/", "us-east-1", "config", expected);
  assert_url ("s3://my-bucket//repo//", "us-east-1", "/config", expected);
  assert_url ("s3://my-bucket/repo", "us-east-1", "//config", expected);

  assert_url ("s3://my-bucket", "us-east-1", NULL, "https://my-bucket.s3.us-east-1.amazonaws.com/");
  assert_url ("s3://my-bucket/", "us-east-1", "", "https://my-bucket.s3.us-east-1.amazonaws.com/");
  assert_url ("s3://my-bucket", "us-east-1", "config",
              "https://my-bucket.s3.us-east-1.amazonaws.com/config");
}

static void
test_error_body (void)
{
  g_autofree char *code = NULL;
  g_autofree char *message = NULL;
  g_autofree char *region = NULL;

  const char *denied = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<Error><Code>AccessDenied</Code><Message>Access &amp; Denied</Message>"
                       "<RequestId>X</RequestId></Error>";
  g_assert_true (_ostree_s3_parse_error_body (denied, strlen (denied), &code, &message, &region));
  g_assert_cmpstr (code, ==, "AccessDenied");
  g_assert_cmpstr (message, ==, "Access & Denied");
  g_assert_null (region);
  g_clear_pointer (&code, g_free);
  g_clear_pointer (&message, g_free);

  const char *redirect = "<Error><Code>PermanentRedirect</Code><Message>m</Message>"
                         "<Endpoint>b.s3.us-west-2.amazonaws.com</Endpoint>"
                         "<Region>us-west-2</Region></Error>";
  g_assert_true (
      _ostree_s3_parse_error_body (redirect, strlen (redirect), &code, &message, &region));
  g_assert_cmpstr (code, ==, "PermanentRedirect");
  g_assert_cmpstr (region, ==, "us-west-2");
  g_clear_pointer (&code, g_free);
  g_clear_pointer (&message, g_free);
  g_clear_pointer (&region, g_free);

  /* NULL out params are allowed */
  g_assert_true (_ostree_s3_parse_error_body (denied, strlen (denied), NULL, NULL, NULL));
}

/* Review focus: whatever a proxy or captive portal sends back must not be
 * mistaken for, or crash on, an S3 error document. */
static void
test_error_body_not_s3 (void)
{
  const char *bodies[] = {
    "",
    "Service Unavailable",
    "<html><body><h1>502 Bad Gateway</h1></body></html>",
    "<Error><Code>AccessDenied</Code><Mess", /* truncated */
    "<Other><Code>AccessDenied</Code></Other>",
    "\xff\xfe\x00garbage",
  };
  for (guint i = 0; i < G_N_ELEMENTS (bodies); i++)
    {
      char *code = (char *)0x1;
      g_assert_false (_ostree_s3_parse_error_body (bodies[i], strlen (bodies[i]), &code, NULL, NULL));
      /* out params are untouched on failure */
      g_assert_true (code == (char *)0x1);
    }
}

/* Review focus: credentials that arrive already close to expiry must be
 * used, not refetched in a loop. */
static void
test_credentials_usable (void)
{
  const guint64 now = 1000000;
  /* never expire */
  g_assert_true (_ostree_s3_credentials_usable (G_MAXUINT64, 0, now));
  /* plenty of time left */
  g_assert_true (_ostree_s3_credentials_usable (now + 3600, now - 1800, now));
  /* inside the refresh window and fetched long ago: refresh */
  g_assert_false (_ostree_s3_credentials_usable (now + 299, now - 1800, now));
  /* inside the refresh window but only just fetched: use them */
  g_assert_true (_ostree_s3_credentials_usable (now + 120, now - 5, now));
  g_assert_true (_ostree_s3_credentials_usable (now + 120, now, now));
  /* ... until the minimum refetch interval has passed */
  g_assert_false (_ostree_s3_credentials_usable (now + 120, now - 60, now));
  /* already expired is never usable, however fresh */
  g_assert_false (_ostree_s3_credentials_usable (now, now, now));
  g_assert_false (_ostree_s3_credentials_usable (now - 1, now, now));
  /* a clock that went backwards must not make stale credentials usable */
  g_assert_false (_ostree_s3_credentials_usable (now + 120, now + 500, now));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/s3/parse/fields", test_parse_fields);
  g_test_add_func ("/s3/parse/rejects", test_parse_rejects);
  g_test_add_func ("/s3/url/addressing", test_url_addressing);
  g_test_add_func ("/s3/url/legacy-buckets", test_url_legacy_buckets);
  g_test_add_func ("/s3/url/key-encoding", test_url_key_encoding);
  g_test_add_func ("/s3/url/slashes", test_url_slashes);
  g_test_add_func ("/s3/error-body/s3", test_error_body);
  g_test_add_func ("/s3/error-body/not-s3", test_error_body_not_s3);
  g_test_add_func ("/s3/credentials-usable", test_credentials_usable);

  return g_test_run ();
}
```

- [ ] **Step 3: Register the test and a stub source**

In `Makefile-tests.am`, find:

```make
if USE_GPGME
_installed_or_uninstalled_test_programs += \
	tests/test-gpg-verify-result \
	$(NULL)
endif
```

Insert immediately after it:

```make

if USE_AWS
_installed_or_uninstalled_test_programs += tests/test-fetcher-s3
tests_test_fetcher_s3_SOURCES = \
	src/libostree/ostree-fetcher-s3.c \
	tests/test-fetcher-s3.c
tests_test_fetcher_s3_CFLAGS = $(TESTS_CFLAGS) $(libglnx_cflags) $(AWS_CFLAGS)
tests_test_fetcher_s3_LDADD = $(TESTS_LDADD) $(AWS_LIBS)
endif
```

Create `src/libostree/ostree-fetcher-s3.c` containing only:

```c
#include "config.h"

#include "ostree-fetcher-s3.h"
```

- [ ] **Step 4: Run the tests to verify they fail**

Run:
```bash
nix develop -c make check TESTS=tests/test-fetcher-s3 2>&1 | tail -15
```
Expected: FAIL at link time with `undefined reference to '_ostree_s3_location_parse'` (and the other four functions).

- [ ] **Step 5: Write the implementation `src/libostree/ostree-fetcher-s3.c`**

Replace the stub with:

```c
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

#include "ostree-fetcher-s3.h"

#include <string.h>

void
_ostree_s3_location_free (OstreeS3Location *loc)
{
  if (!loc)
    return;
  g_free (loc->bucket);
  g_free (loc->key_prefix);
  g_free (loc->region);
  g_free (loc->profile);
  g_free (loc->endpoint);
  g_free (loc);
}

static gboolean
throw_invalid (GError **error, const char *uri, const char *reason)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid s3:// URL %s: %s", uri,
               reason);
  return FALSE;
}

/* Regions end up in a hostname, so be strict about what they contain. */
static gboolean
region_is_valid (const char *region)
{
  if (*region == '\0')
    return FALSE;
  for (const char *p = region; *p; p++)
    {
      if (!(g_ascii_islower (*p) || g_ascii_isdigit (*p) || *p == '-'))
        return FALSE;
    }
  return TRUE;
}

/* Whether @bucket can be used as a single TLS-matchable DNS label, i.e.
 * with virtual-hosted addressing. Dots break wildcard certificate matching;
 * uppercase and underscores exist in legacy buckets and are not valid labels. */
static gboolean
bucket_is_dns_safe (const char *bucket)
{
  const gsize len = strlen (bucket);
  if (len < 3 || len > 63)
    return FALSE;
  for (const char *p = bucket; *p; p++)
    {
      if (!(g_ascii_islower (*p) || g_ascii_isdigit (*p) || *p == '-'))
        return FALSE;
    }
  return TRUE;
}

/* Returns scheme://host[:port], or NULL if @value is not a bare http(s) origin. */
static char *
normalize_endpoint (const char *value)
{
  g_autoptr (GUri) u = g_uri_parse (value, G_URI_FLAGS_NONE, NULL);
  if (!u)
    return NULL;
  const char *scheme = g_uri_get_scheme (u);
  const char *host = g_uri_get_host (u);
  const char *path = g_uri_get_path (u);
  if (!(g_str_equal (scheme, "http") || g_str_equal (scheme, "https")))
    return NULL;
  if (host == NULL || *host == '\0')
    return NULL;
  if (g_uri_get_userinfo (u) != NULL || g_uri_get_query (u) != NULL
      || g_uri_get_fragment (u) != NULL)
    return NULL;
  if (!(*path == '\0' || g_str_equal (path, "/")))
    return NULL;
  if (g_uri_get_port (u) != -1)
    return g_strdup_printf ("%s://%s:%d", scheme, host, g_uri_get_port (u));
  return g_strdup_printf ("%s://%s", scheme, host);
}

OstreeS3Location *
_ostree_s3_location_parse (GUri *uri, GError **error)
{
  g_autofree char *uri_str = g_uri_to_string_partial (uri, G_URI_HIDE_PASSWORD);

  if (!g_str_equal (g_uri_get_scheme (uri), "s3"))
    return throw_invalid (error, uri_str, "scheme is not s3"), NULL;

  const char *host = g_uri_get_host (uri);
  if (host == NULL || *host == '\0' || strchr (host, '%') != NULL)
    return throw_invalid (error, uri_str, "missing or malformed bucket name"), NULL;
  if (g_uri_get_userinfo (uri) != NULL || g_uri_get_user (uri) != NULL)
    return throw_invalid (error, uri_str,
                          "credentials are not allowed in the URL; they come from the AWS "
                          "credential chain"),
           NULL;
  if (g_uri_get_port (uri) != -1)
    return throw_invalid (error, uri_str, "a port is not allowed; use ?endpoint= instead"), NULL;
  if (g_uri_get_fragment (uri) != NULL)
    return throw_invalid (error, uri_str, "a fragment is not allowed"), NULL;

  g_autoptr (OstreeS3Location) loc = g_new0 (OstreeS3Location, 1);
  loc->bucket = g_strdup (host);

  g_autofree char *path = g_uri_unescape_string (g_uri_get_path (uri), NULL);
  if (path == NULL)
    return throw_invalid (error, uri_str, "invalid percent-encoding in path"), NULL;
  const char *start = path;
  while (*start == '/')
    start++;
  gsize len = strlen (start);
  while (len > 0 && start[len - 1] == '/')
    len--;
  loc->key_prefix = g_strndup (start, len);

  const char *query = g_uri_get_query (uri);
  if (query != NULL && *query != '\0')
    {
      GUriParamsIter iter;
      char *attr_raw = NULL;
      char *value_raw = NULL;
      g_autoptr (GError) local_error = NULL;

      g_uri_params_iter_init (&iter, query, -1, "&", G_URI_PARAMS_NONE);
      while (g_uri_params_iter_next (&iter, &attr_raw, &value_raw, &local_error))
        {
          g_autofree char *attr = attr_raw;
          g_autofree char *value = value_raw;
          char **slot = NULL;

          if (g_str_equal (attr, "region"))
            slot = &loc->region;
          else if (g_str_equal (attr, "profile"))
            slot = &loc->profile;
          else if (g_str_equal (attr, "endpoint"))
            slot = &loc->endpoint;
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Invalid s3:// URL %s: Unknown parameter '%s' (allowed: region, "
                           "profile, endpoint)",
                           uri_str, attr);
              return NULL;
            }
          if (*slot != NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Invalid s3:// URL %s: parameter '%s' given more than once", uri_str,
                           attr);
              return NULL;
            }
          if (*value == '\0')
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Invalid s3:// URL %s: parameter '%s' is empty", uri_str, attr);
              return NULL;
            }
          *slot = g_steal_pointer (&value);
        }
      if (local_error != NULL)
        return throw_invalid (error, uri_str, local_error->message), NULL;
    }

  if (loc->region != NULL && !region_is_valid (loc->region))
    return throw_invalid (error, uri_str, "region may only contain a-z, 0-9 and '-'"), NULL;

  if (loc->endpoint != NULL)
    {
      g_autofree char *normalized = normalize_endpoint (loc->endpoint);
      if (normalized == NULL)
        return throw_invalid (error, uri_str, "endpoint must look like http(s)://host[:port]"),
               NULL;
      g_free (loc->endpoint);
      loc->endpoint = g_steal_pointer (&normalized);
    }

  return g_steal_pointer (&loc);
}

/* Append each non-empty '/'-separated segment of @path to @out as
 * "/<segment>", percent-encoding everything outside the SigV4 unreserved
 * set (A-Z a-z 0-9 - . _ ~) exactly once. */
static void
append_escaped_segments (GString *out, const char *path)
{
  if (path == NULL)
    return;
  g_auto (GStrv) segments = g_strsplit (path, "/", -1);
  for (char **seg = segments; *seg != NULL; seg++)
    {
      if (**seg == '\0')
        continue;
      g_autofree char *escaped = g_uri_escape_string (*seg, NULL, FALSE);
      g_string_append_c (out, '/');
      g_string_append (out, escaped);
    }
}

char *
_ostree_s3_location_to_request_url (const OstreeS3Location *loc, const char *region,
                                    const char *subpath)
{
  g_autoptr (GString) url = g_string_new (NULL);
  gboolean path_style;

  if (loc->endpoint != NULL)
    {
      g_string_append (url, loc->endpoint);
      path_style = TRUE;
    }
  else
    {
      g_assert (region != NULL);
      path_style = !bucket_is_dns_safe (loc->bucket);
      if (path_style)
        g_string_append_printf (url, "https://s3.%s.amazonaws.com", region);
      else
        g_string_append_printf (url, "https://%s.s3.%s.amazonaws.com", loc->bucket, region);
    }

  const gsize origin_len = url->len;
  if (path_style)
    append_escaped_segments (url, loc->bucket);
  append_escaped_segments (url, loc->key_prefix);
  append_escaped_segments (url, subpath);
  if (url->len == origin_len)
    g_string_append_c (url, '/');

  return g_string_free (g_steal_pointer (&url), FALSE);
}

typedef struct
{
  int depth;
  gboolean is_error_doc;
  GString *text;
  char *code;
  char *message;
  char *region;
} S3ErrorParse;

static void
s3_error_start_element (GMarkupParseContext *context, const char *element_name,
                        const char **attribute_names, const char **attribute_values,
                        gpointer user_data, GError **error)
{
  S3ErrorParse *parse = user_data;
  parse->depth++;
  if (parse->depth == 1)
    parse->is_error_doc = g_str_equal (element_name, "Error");
  g_string_truncate (parse->text, 0);
}

static void
s3_error_text (GMarkupParseContext *context, const char *text, gsize text_len, gpointer user_data,
               GError **error)
{
  S3ErrorParse *parse = user_data;
  g_string_append_len (parse->text, text, text_len);
}

static void
s3_error_end_element (GMarkupParseContext *context, const char *element_name, gpointer user_data,
                      GError **error)
{
  S3ErrorParse *parse = user_data;
  if (parse->is_error_doc && parse->depth == 2)
    {
      char **slot = NULL;
      if (g_str_equal (element_name, "Code"))
        slot = &parse->code;
      else if (g_str_equal (element_name, "Message"))
        slot = &parse->message;
      else if (g_str_equal (element_name, "Region"))
        slot = &parse->region;
      if (slot != NULL && *slot == NULL)
        *slot = g_strdup (parse->text->str);
    }
  parse->depth--;
}

gboolean
_ostree_s3_parse_error_body (const char *body, gsize len, char **out_code, char **out_message,
                             char **out_region)
{
  static const GMarkupParser parser
      = { s3_error_start_element, s3_error_end_element, s3_error_text, NULL, NULL };
  S3ErrorParse parse = { 0 };
  parse.text = g_string_new (NULL);
  g_autoptr (GMarkupParseContext) context = g_markup_parse_context_new (&parser, 0, &parse, NULL);

  gboolean ok = g_markup_parse_context_parse (context, body, len, NULL)
                && g_markup_parse_context_end_parse (context, NULL) && parse.is_error_doc;
  g_string_free (parse.text, TRUE);

  if (ok)
    {
      if (out_code)
        *out_code = g_steal_pointer (&parse.code);
      if (out_message)
        *out_message = g_steal_pointer (&parse.message);
      if (out_region)
        *out_region = g_steal_pointer (&parse.region);
    }
  g_free (parse.code);
  g_free (parse.message);
  g_free (parse.region);
  return ok;
}

gboolean
_ostree_s3_credentials_usable (guint64 expiry, guint64 fetched_at, guint64 now)
{
  if (expiry == G_MAXUINT64)
    return TRUE;
  if (expiry <= now)
    return FALSE;
  if (expiry - now > OSTREE_S3_CREDS_REFRESH_WINDOW_SECS)
    return TRUE;
  /* Close to expiry. If we only just fetched them, the provider has nothing
   * fresher to give, so use them rather than refetching in a loop. */
  return now >= fetched_at && now - fetched_at < OSTREE_S3_CREDS_MIN_REFETCH_INTERVAL_SECS;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
nix develop -c bash -c 'make check TESTS=tests/test-fetcher-s3 2>&1 | grep -E "^(PASS|FAIL|ERROR|# (TOTAL|PASS|FAIL))"'
```
Expected: nine `PASS:` lines (`/s3/credentials-usable`, `/s3/parse/fields`, `/s3/parse/rejects`, `/s3/url/addressing`, `/s3/url/legacy-buckets`, `/s3/url/key-encoding`, `/s3/url/slashes`, `/s3/error-body/s3`, `/s3/error-body/not-s3`), `# FAIL:  0`.

- [ ] **Step 7: Add the unit to libostree**

In `Makefile-libostree.am`, find:

```make
if USE_CURL
libostree_1_la_SOURCES += src/libostree/ostree-fetcher-curl.c \
  $(NULL)
libostree_1_la_CFLAGS += $(OT_DEP_CURL_CFLAGS)
libostree_1_la_LIBADD += $(OT_DEP_CURL_LIBS)
```

Insert immediately after those five lines (before the `else`):

```make
if USE_AWS
libostree_1_la_SOURCES += \
	src/libostree/ostree-fetcher-s3.h \
	src/libostree/ostree-fetcher-s3.c \
	$(NULL)
libostree_1_la_CFLAGS += $(AWS_CFLAGS)
libostree_1_la_LIBADD += $(AWS_LIBS)
endif
```

Run:
```bash
nix develop -c bash -c 'make -j"$(nproc)" 2>&1 | tail -3'
```
Expected: build succeeds with no warnings from `ostree-fetcher-s3.c`.

- [ ] **Step 8: Commit**

```bash
git add src/libostree/ostree-fetcher-s3.h src/libostree/ostree-fetcher-s3.c tests/test-fetcher-s3.c Makefile-libostree.am Makefile-tests.am
git commit -m "lib/fetcher: add s3:// URL parsing and request URL construction

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: `ostree-aws` unit

**Files:**
- Create: `src/libostree/ostree-aws.h`
- Create: `src/libostree/ostree-aws.c`
- Modify: `tests/test-fetcher-s3.c`
- Modify: `Makefile-libostree.am`, `Makefile-tests.am` (the blocks added in Task 3)

**Interfaces:**
- Consumes: nothing from `ostree-fetcher-s3`; the build glue from Tasks 2-3.
- Produces (all in `ostree-aws.h`):
  - `typedef struct OstreeAwsCredentials OstreeAwsCredentials;` with `g_autoptr` support
  - `OstreeAwsCredentials *_ostree_aws_credentials_ref (OstreeAwsCredentials *creds);`
  - `void _ostree_aws_credentials_unref (OstreeAwsCredentials *creds);` (NULL-safe)
  - `guint64 _ostree_aws_credentials_get_expiry (OstreeAwsCredentials *creds);`
  - `OstreeAwsCredentials *_ostree_aws_credentials_new_static (const char *access_key_id, const char *secret_access_key, const char *session_token);`
  - `char *_ostree_aws_resolve_region (const char *url_region, const char *profile, GCancellable *cancellable, GError **error);`
  - `void _ostree_aws_get_credentials_async (const char *profile, const char *region, gboolean force_refresh, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);`
  - `OstreeAwsCredentials *_ostree_aws_get_credentials_finish (GAsyncResult *result, GError **error);`
  - `gboolean _ostree_aws_sign_get (OstreeAwsCredentials *creds, const char *url, const char *region, GPtrArray *headers, GError **error);`
  - `gboolean _ostree_aws_sign_get_at (OstreeAwsCredentials *creds, const char *url, const char *region, guint64 unix_time, GPtrArray *headers, GError **error);`

  Contract details later tasks rely on:
  - The async call never invokes its callback before returning; it completes on the caller's thread-default main context.
  - Two results for the same `(profile, region)` without `force_refresh` are the same underlying credentials but distinct `OstreeAwsCredentials` objects.
  - `headers` receives `"Name: value"` strings owned by the array (create it with `g_ptr_array_new_with_free_func (g_free)`).
  - A credentials failure is `G_IO_ERROR_PERMISSION_DENIED`; an unresolvable region is `G_IO_ERROR_INVALID_ARGUMENT`.

- [ ] **Step 1: Write the header `src/libostree/ostree-aws.h`**

```c
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

/* Refcounted, immutable set of AWS credentials. */
typedef struct OstreeAwsCredentials OstreeAwsCredentials;

OstreeAwsCredentials *_ostree_aws_credentials_ref (OstreeAwsCredentials *creds);
void _ostree_aws_credentials_unref (OstreeAwsCredentials *creds);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeAwsCredentials, _ostree_aws_credentials_unref)

/* Seconds since the epoch; G_MAXUINT64 for non-expiring credentials. */
guint64 _ostree_aws_credentials_get_expiry (OstreeAwsCredentials *creds);

/* Fixed credentials, bypassing the provider chain. Used by tests.
 * @session_token may be NULL. */
OstreeAwsCredentials *_ostree_aws_credentials_new_static (const char *access_key_id,
                                                          const char *secret_access_key,
                                                          const char *session_token);

/* Resolve the region: @url_region, then AWS_REGION, AWS_DEFAULT_REGION, the
 * profile's region, then IMDS. @url_region and @profile may be NULL. Blocks
 * only for the IMDS fallback. */
char *_ostree_aws_resolve_region (const char *url_region, const char *profile,
                                  GCancellable *cancellable, GError **error);

/* Async. Completes on the thread-default main context of the caller.
 * @profile may be NULL. @force_refresh bypasses the process-wide cache. */
void _ostree_aws_get_credentials_async (const char *profile, const char *region,
                                        gboolean force_refresh, GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer user_data);
OstreeAwsCredentials *_ostree_aws_get_credentials_finish (GAsyncResult *result, GError **error);

/* Synchronous. Signs a bodyless GET for @url (already percent-encoded) and
 * appends the resulting headers, as "Name: value" strings, to @headers. */
gboolean _ostree_aws_sign_get (OstreeAwsCredentials *creds, const char *url, const char *region,
                               GPtrArray *headers, GError **error);

/* As above with an explicit timestamp (seconds since the epoch). Used by tests. */
gboolean _ostree_aws_sign_get_at (OstreeAwsCredentials *creds, const char *url, const char *region,
                                  guint64 unix_time, GPtrArray *headers, GError **error);

G_END_DECLS
```

- [ ] **Step 2: Add the failing tests to `tests/test-fetcher-s3.c`**

Add the include, directly under `#include "config.h"` and its blank line:

```c
#include "ostree-aws.h"
```

Add `#include <glib/gstdio.h>` after `#include <glib.h>`.

Insert this block immediately before `int\nmain (int argc, char **argv)`:

```c
/* ---- ostree-aws ---- */

static const char *
find_header (GPtrArray *headers, const char *name)
{
  const gsize len = strlen (name);
  for (guint i = 0; i < headers->len; i++)
    {
      const char *h = headers->pdata[i];
      if (g_ascii_strncasecmp (h, name, len) == 0 && h[len] == ':')
        return h + len + 2;
    }
  return NULL;
}

/* Expected signatures were computed with an independent SigV4
 * implementation (Python hashlib/hmac), not with aws-c-auth. */
static void
test_sign (void)
{
  const guint64 t = 1369353600; /* 20130524T000000Z */
  g_autoptr (GError) error = NULL;

  g_autoptr (OstreeAwsCredentials) creds = _ostree_aws_credentials_new_static (
      "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", NULL);
  g_assert_cmpuint (_ostree_aws_credentials_get_expiry (creds), ==, G_MAXUINT64);
  g_autoptr (GPtrArray) headers = g_ptr_array_new_with_free_func (g_free);
  _ostree_aws_sign_get_at (creds,
                           "https://examplebucket.s3.amazonaws.com/repo/deltas/ab/c%2Bd/superblock",
                           "us-east-1", t, headers, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (find_header (headers, "Authorization"), ==,
                   "AWS4-HMAC-SHA256 "
                   "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
                   "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
                   "Signature=1f1b4657c75f09877e88547ee17c26163a6c952895a7b120ef92e311633ed4e7");
  g_assert_cmpstr (find_header (headers, "x-amz-date"), ==, "20130524T000000Z");
  g_assert_cmpstr (find_header (headers, "x-amz-content-sha256"), ==, "UNSIGNED-PAYLOAD");
  g_assert_null (find_header (headers, "x-amz-security-token"));

  g_autoptr (OstreeAwsCredentials) temp_creds = _ostree_aws_credentials_new_static (
      "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "TOKENEXAMPLE");
  g_autoptr (GPtrArray) temp_headers = g_ptr_array_new_with_free_func (g_free);
  _ostree_aws_sign_get_at (temp_creds, "https://examplebucket.s3.amazonaws.com/repo/config",
                           "us-east-1", t, temp_headers, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (find_header (temp_headers, "Authorization"), ==,
                   "AWS4-HMAC-SHA256 "
                   "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
                   "SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-security-token, "
                   "Signature=d1670c1ba4ed45429c40b210bd45785d7a9fd2181a243a18cba3c25d55fa9c8f");
  g_assert_cmpstr (find_header (temp_headers, "x-amz-security-token"), ==, "TOKENEXAMPLE");
}

/* A non-default port is part of the signed host header. */
static void
test_sign_port (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeAwsCredentials) creds = _ostree_aws_credentials_new_static ("AK", "SK", NULL);
  g_autoptr (GPtrArray) with_port = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) default_port = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) no_port = g_ptr_array_new_with_free_func (g_free);
  _ostree_aws_sign_get_at (creds, "http://127.0.0.1:9000/b/k", "us-east-1", 1, with_port, &error);
  g_assert_no_error (error);
  _ostree_aws_sign_get_at (creds, "http://127.0.0.1:80/b/k", "us-east-1", 1, default_port, &error);
  g_assert_no_error (error);
  _ostree_aws_sign_get_at (creds, "http://127.0.0.1/b/k", "us-east-1", 1, no_port, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (find_header (default_port, "Authorization"), ==,
                   find_header (no_port, "Authorization"));
  g_assert_cmpstr (find_header (with_port, "Authorization"), !=,
                   find_header (no_port, "Authorization"));
}

static void
clear_aws_env (const char *tmpdir)
{
  static const char *const names[]
      = { "AWS_REGION",          "AWS_DEFAULT_REGION",          "AWS_PROFILE",
          "AWS_ACCESS_KEY_ID",   "AWS_SECRET_ACCESS_KEY",       "AWS_SESSION_TOKEN",
          "AWS_ROLE_ARN",        "AWS_WEB_IDENTITY_TOKEN_FILE", "AWS_CONTAINER_CREDENTIALS_FULL_URI",
          "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI" };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    g_unsetenv (names[i]);
  g_autofree char *config = g_build_filename (tmpdir, "config", NULL);
  g_autofree char *credentials = g_build_filename (tmpdir, "credentials", NULL);
  g_setenv ("AWS_CONFIG_FILE", config, TRUE);
  g_setenv ("AWS_SHARED_CREDENTIALS_FILE", credentials, TRUE);
  g_setenv ("AWS_EC2_METADATA_DISABLED", "true", TRUE);
  g_unlink (config);
  g_unlink (credentials);
}

static void
test_resolve_region (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *tmpdir = g_dir_make_tmp ("test-fetcher-s3-XXXXXX", &error);
  g_assert_no_error (error);
  clear_aws_env (tmpdir);
  g_autofree char *config = g_build_filename (tmpdir, "config", NULL);

  /* nothing configured */
  g_autofree char *none = _ostree_aws_resolve_region (NULL, NULL, NULL, &error);
  g_assert_null (none);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "?region="));
  g_clear_error (&error);

  /* profile */
  g_file_set_contents (config,
                       "[default]\nregion = eu-west-1\n\n[profile gov]\nregion = us-gov-east-1\n",
                       -1, &error);
  g_assert_no_error (error);
  g_autofree char *from_default = _ostree_aws_resolve_region (NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_default, ==, "eu-west-1");
  g_autofree char *from_named = _ostree_aws_resolve_region (NULL, "gov", NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_named, ==, "us-gov-east-1");
  g_setenv ("AWS_PROFILE", "gov", TRUE);
  g_autofree char *from_env_profile = _ostree_aws_resolve_region (NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_env_profile, ==, "us-gov-east-1");

  /* AWS_DEFAULT_REGION beats the profile */
  g_setenv ("AWS_DEFAULT_REGION", "ap-south-1", TRUE);
  g_autofree char *from_default_env = _ostree_aws_resolve_region (NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_default_env, ==, "ap-south-1");

  /* AWS_REGION beats AWS_DEFAULT_REGION */
  g_setenv ("AWS_REGION", "us-gov-west-1", TRUE);
  g_autofree char *from_region_env = _ostree_aws_resolve_region (NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_region_env, ==, "us-gov-west-1");

  /* the URL beats everything */
  g_autofree char *from_url = _ostree_aws_resolve_region ("us-east-2", NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_url, ==, "us-east-2");

  g_unlink (config);
  g_rmdir (tmpdir);
}

static void
on_creds (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GAsyncResult **out = user_data;
  *out = g_object_ref (result);
  g_main_context_wakeup (NULL);
}

static OstreeAwsCredentials *
get_creds_sync (const char *region, gboolean force, GError **error)
{
  g_autoptr (GAsyncResult) result = NULL;
  _ostree_aws_get_credentials_async (NULL, region, force, NULL, on_creds, &result);
  /* Must never complete before returning to the main loop. */
  g_assert_null (result);
  while (result == NULL)
    g_main_context_iteration (NULL, TRUE);
  return _ostree_aws_get_credentials_finish (result, error);
}

static void
test_credentials_chain (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *tmpdir = g_dir_make_tmp ("test-fetcher-s3-XXXXXX", &error);
  g_assert_no_error (error);
  clear_aws_env (tmpdir);

  /* Each case uses its own region so it gets its own cached chain. */
  g_autoptr (OstreeAwsCredentials) missing = get_creds_sync ("test-none-1", FALSE, &error);
  g_assert_null (missing);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_nonnull (strstr (error->message, "No AWS credentials available"));
  g_assert_nonnull (strstr (error->message, "profile 'default'"));
  g_clear_error (&error);

  g_setenv ("AWS_ACCESS_KEY_ID", "AKIDFIRST", TRUE);
  g_setenv ("AWS_SECRET_ACCESS_KEY", "secret", TRUE);
  g_autoptr (OstreeAwsCredentials) first = get_creds_sync ("test-env-1", FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);
  g_assert_cmpuint (_ostree_aws_credentials_get_expiry (first), ==, G_MAXUINT64);

  /* Cached: a changed environment is not seen without force_refresh ... */
  g_autoptr (GPtrArray) h_first = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) h_cached = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) h_forced = g_ptr_array_new_with_free_func (g_free);
  g_setenv ("AWS_ACCESS_KEY_ID", "AKIDSECOND", TRUE);
  g_autoptr (OstreeAwsCredentials) cached = get_creds_sync ("test-env-1", FALSE, &error);
  g_assert_no_error (error);
  /* ... and is seen with it. */
  g_autoptr (OstreeAwsCredentials) forced = get_creds_sync ("test-env-1", TRUE, &error);
  g_assert_no_error (error);

  const char *url = "https://b.s3.us-east-1.amazonaws.com/k";
  g_assert_true (_ostree_aws_sign_get_at (first, url, "us-east-1", 1, h_first, &error));
  g_assert_true (_ostree_aws_sign_get_at (cached, url, "us-east-1", 1, h_cached, &error));
  g_assert_true (_ostree_aws_sign_get_at (forced, url, "us-east-1", 1, h_forced, &error));
  g_assert_nonnull (strstr (find_header (h_first, "Authorization"), "Credential=AKIDFIRST/"));
  g_assert_nonnull (strstr (find_header (h_cached, "Authorization"), "Credential=AKIDFIRST/"));
  g_assert_nonnull (strstr (find_header (h_forced, "Authorization"), "Credential=AKIDSECOND/"));

  g_rmdir (tmpdir);
}
```

In `main`, add these lines before `return g_test_run ();`:

```c
  g_test_add_func ("/aws/sign", test_sign);
  g_test_add_func ("/aws/sign-port", test_sign_port);
  g_test_add_func ("/aws/resolve-region", test_resolve_region);
  g_test_add_func ("/aws/credentials-chain", test_credentials_chain);
```

- [ ] **Step 3: Add the source to both makefiles, with a stub**

In `Makefile-tests.am`, change the Task 3 block's source list to:

```make
tests_test_fetcher_s3_SOURCES = \
	src/libostree/ostree-fetcher-s3.c \
	src/libostree/ostree-aws.c \
	tests/test-fetcher-s3.c
```

In `Makefile-libostree.am`, change the Task 3 block's source list to:

```make
libostree_1_la_SOURCES += \
	src/libostree/ostree-fetcher-s3.h \
	src/libostree/ostree-fetcher-s3.c \
	src/libostree/ostree-aws.h \
	src/libostree/ostree-aws.c \
	$(NULL)
```

Create `src/libostree/ostree-aws.c` containing only:

```c
#include "config.h"

#include "ostree-aws.h"
```

- [ ] **Step 4: Run the tests to verify they fail**

Run:
```bash
nix develop -c make check TESTS=tests/test-fetcher-s3 2>&1 | tail -15
```
Expected: FAIL at link time with `undefined reference to '_ostree_aws_sign_get_at'` (and the other `_ostree_aws_*` functions).

- [ ] **Step 5: Write the implementation `src/libostree/ostree-aws.c`**

Replace the stub with:

```c
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

#include "ostree-aws.h"

#include <aws/auth/auth.h>
#include <aws/auth/aws_imds_client.h>
#include <aws/auth/credentials.h>
#include <aws/auth/signable.h>
#include <aws/auth/signing.h>
#include <aws/auth/signing_config.h>
#include <aws/auth/signing_result.h>
#include <aws/common/date_time.h>
#include <aws/common/string.h>
#include <aws/http/request_response.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/sdkutils/aws_profile.h>

#include <string.h>

/* How long the caching provider keeps credentials that carry no expiry. */
#define OSTREE_AWS_CACHE_REFRESH_MS (15 * 60 * 1000)
/* How long to wait for IMDS when it is the last resort for the region. */
#define OSTREE_AWS_IMDS_REGION_TIMEOUT_SECS 10

struct OstreeAwsCredentials
{
  gatomicrefcount refcount;
  struct aws_credentials *creds;
};

typedef struct
{
  struct aws_credentials_provider *provider;
  gboolean has_sso;
} ProviderEntry;

typedef struct
{
  struct aws_allocator *alloc;
  struct aws_client_bootstrap *bootstrap;
  struct aws_tls_ctx *tls_ctx; /* NULL if TLS setup failed; HTTPS-based providers are skipped */
  GMutex lock;
  GHashTable *providers; /* "profile\nregion" -> ProviderEntry*, protected by lock */
} OstreeAwsRuntime;

/* Process-wide AWS runtime, started on first use and never torn down. */
static OstreeAwsRuntime *
get_runtime (void)
{
  static gsize initialized;
  static OstreeAwsRuntime runtime;

  if (g_once_init_enter (&initialized))
    {
      OstreeAwsRuntime *rt = &runtime;
      rt->alloc = aws_default_allocator ();
      aws_auth_library_init (rt->alloc);

      struct aws_event_loop_group_options elg_opts = { .loop_count = 1 };
      struct aws_event_loop_group *elg = aws_event_loop_group_new (rt->alloc, &elg_opts);
      g_assert (elg != NULL);

      struct aws_host_resolver_default_options resolver_opts
          = { .max_entries = 8, .el_group = elg };
      struct aws_host_resolver *resolver = aws_host_resolver_new_default (rt->alloc, &resolver_opts);
      g_assert (resolver != NULL);

      struct aws_client_bootstrap_options bootstrap_opts
          = { .event_loop_group = elg, .host_resolver = resolver };
      rt->bootstrap = aws_client_bootstrap_new (rt->alloc, &bootstrap_opts);
      g_assert (rt->bootstrap != NULL);

      struct aws_tls_ctx_options tls_opts;
      aws_tls_ctx_options_init_default_client (&tls_opts, rt->alloc);
      rt->tls_ctx = aws_tls_client_ctx_new (rt->alloc, &tls_opts);
      aws_tls_ctx_options_clean_up (&tls_opts);
      if (rt->tls_ctx == NULL)
        g_debug ("aws: no TLS context; SSO, web identity and ECS credentials are unavailable");

      g_mutex_init (&rt->lock);
      rt->providers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

      g_once_init_leave (&initialized, 1);
    }
  return &runtime;
}

static gboolean
imds_disabled (void)
{
  const char *v = g_getenv ("AWS_EC2_METADATA_DISABLED");
  return v != NULL && g_ascii_strcasecmp (v, "true") == 0;
}

static gboolean
region_is_valid (const char *region)
{
  if (region == NULL || *region == '\0')
    return FALSE;
  for (const char *p = region; *p; p++)
    {
      if (!(g_ascii_islower (*p) || g_ascii_isdigit (*p) || *p == '-'))
        return FALSE;
    }
  return TRUE;
}

/* ---- credentials ---- */

static OstreeAwsCredentials *
credentials_wrap (struct aws_credentials *creds)
{
  OstreeAwsCredentials *ret = g_new0 (OstreeAwsCredentials, 1);
  g_atomic_ref_count_init (&ret->refcount);
  aws_credentials_acquire (creds);
  ret->creds = creds;
  return ret;
}

OstreeAwsCredentials *
_ostree_aws_credentials_ref (OstreeAwsCredentials *creds)
{
  g_atomic_ref_count_inc (&creds->refcount);
  return creds;
}

void
_ostree_aws_credentials_unref (OstreeAwsCredentials *creds)
{
  if (creds == NULL)
    return;
  if (g_atomic_ref_count_dec (&creds->refcount))
    {
      aws_credentials_release (creds->creds);
      g_free (creds);
    }
}

guint64
_ostree_aws_credentials_get_expiry (OstreeAwsCredentials *creds)
{
  return aws_credentials_get_expiration_timepoint_seconds (creds->creds);
}

OstreeAwsCredentials *
_ostree_aws_credentials_new_static (const char *access_key_id, const char *secret_access_key,
                                    const char *session_token)
{
  OstreeAwsRuntime *rt = get_runtime ();
  struct aws_byte_cursor token = { 0 };
  if (session_token != NULL)
    token = aws_byte_cursor_from_c_str (session_token);
  struct aws_credentials *creds
      = aws_credentials_new (rt->alloc, aws_byte_cursor_from_c_str (access_key_id),
                             aws_byte_cursor_from_c_str (secret_access_key), token, UINT64_MAX);
  g_assert (creds != NULL);
  OstreeAwsCredentials *ret = credentials_wrap (creds);
  aws_credentials_release (creds);
  return ret;
}

/* Build: environment -> SSO -> profile -> web identity -> ECS or IMDS,
 * wrapped in a caching provider. aws-c-auth's own default chain omits SSO. */
static struct aws_credentials_provider *
build_chain (OstreeAwsRuntime *rt, const char *profile, const char *region, gboolean *out_has_sso)
{
  struct aws_credentials_provider *providers[5];
  size_t n = 0;
  struct aws_credentials_provider *p;
  struct aws_byte_cursor profile_cur = { 0 };
  if (profile != NULL)
    profile_cur = aws_byte_cursor_from_c_str (profile);

  *out_has_sso = FALSE;

  struct aws_credentials_provider_environment_options env_opts;
  AWS_ZERO_STRUCT (env_opts);
  p = aws_credentials_provider_new_environment (rt->alloc, &env_opts);
  if (p != NULL)
    providers[n++] = p;

  if (rt->tls_ctx != NULL)
    {
      /* Returns NULL when the profile has no SSO configuration. */
      struct aws_credentials_provider_sso_options sso_opts;
      AWS_ZERO_STRUCT (sso_opts);
      sso_opts.profile_name_override = profile_cur;
      sso_opts.bootstrap = rt->bootstrap;
      sso_opts.tls_ctx = rt->tls_ctx;
      p = aws_credentials_provider_new_sso (rt->alloc, &sso_opts);
      if (p != NULL)
        {
          providers[n++] = p;
          *out_has_sso = TRUE;
        }
    }

  struct aws_credentials_provider_profile_options profile_opts;
  AWS_ZERO_STRUCT (profile_opts);
  profile_opts.profile_name_override = profile_cur;
  profile_opts.bootstrap = rt->bootstrap;
  profile_opts.tls_ctx = rt->tls_ctx;
  p = aws_credentials_provider_new_profile (rt->alloc, &profile_opts);
  if (p != NULL)
    providers[n++] = p;

  if (rt->tls_ctx != NULL)
    {
      /* The region is passed explicitly: without one this provider fails
       * outright in IRSA setups that only set the role and token file. */
      struct aws_credentials_provider_sts_web_identity_options web_opts;
      AWS_ZERO_STRUCT (web_opts);
      web_opts.bootstrap = rt->bootstrap;
      web_opts.tls_ctx = rt->tls_ctx;
      web_opts.profile_name_override = profile_cur;
      web_opts.region = aws_byte_cursor_from_c_str (region);
      p = aws_credentials_provider_new_sts_web_identity (rt->alloc, &web_opts);
      if (p != NULL)
        providers[n++] = p;
    }

  struct aws_credentials_provider_ecs_environment_options ecs_opts;
  AWS_ZERO_STRUCT (ecs_opts);
  ecs_opts.bootstrap = rt->bootstrap;
  ecs_opts.tls_ctx = rt->tls_ctx;
  p = aws_credentials_provider_new_ecs_from_environment (rt->alloc, &ecs_opts);
  if (p == NULL && !imds_disabled ())
    {
      struct aws_credentials_provider_imds_options imds_opts;
      AWS_ZERO_STRUCT (imds_opts);
      imds_opts.bootstrap = rt->bootstrap;
      imds_opts.imds_version = IMDS_PROTOCOL_V2;
      p = aws_credentials_provider_new_imds (rt->alloc, &imds_opts);
    }
  if (p != NULL)
    providers[n++] = p;

  struct aws_credentials_provider_chain_options chain_opts
      = { .providers = providers, .provider_count = n };
  struct aws_credentials_provider *chain = aws_credentials_provider_new_chain (rt->alloc, &chain_opts);
  for (size_t i = 0; i < n; i++)
    aws_credentials_provider_release (providers[i]);
  if (chain == NULL)
    return NULL;

  struct aws_credentials_provider_cached_options cached_opts;
  AWS_ZERO_STRUCT (cached_opts);
  cached_opts.source = chain;
  cached_opts.refresh_time_in_milliseconds = OSTREE_AWS_CACHE_REFRESH_MS;
  struct aws_credentials_provider *cached
      = aws_credentials_provider_new_cached (rt->alloc, &cached_opts);
  aws_credentials_provider_release (chain);
  return cached;
}

typedef struct
{
  char *profile_display;
  gboolean has_sso;
} CredsTaskData;

static void
creds_task_data_free (CredsTaskData *data)
{
  g_free (data->profile_display);
  g_free (data);
}

/* May run on the AWS event-loop thread or synchronously on the caller's
 * thread; GTask delivers the result on the caller's main context either way. */
static void
on_get_credentials (struct aws_credentials *creds, int error_code, void *user_data)
{
  g_autoptr (GTask) task = G_TASK (user_data);
  CredsTaskData *data = g_task_get_task_data (task);

  if (creds != NULL)
    {
      g_task_return_pointer (task, credentials_wrap (creds),
                             (GDestroyNotify)_ostree_aws_credentials_unref);
      return;
    }

  g_autoptr (GString) msg = g_string_new (NULL);
  g_string_append_printf (msg,
                          "No AWS credentials available for s3:// remote (tried environment, "
                          "%sshared config profile '%s', web identity, ECS/IMDS): %s",
                          data->has_sso ? "SSO, " : "", data->profile_display,
                          aws_error_str (error_code));
  if (data->has_sso)
    g_string_append_printf (msg,
                            ". Profile '%s' uses SSO: if the SSO session has expired, run "
                            "`aws sso login --profile %s`, or configure the profile with "
                            "`credential_process = aws configure export-credentials --profile %s "
                            "--format process`",
                            data->profile_display, data->profile_display, data->profile_display);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s", msg->str);
}

void
_ostree_aws_get_credentials_async (const char *profile, const char *region, gboolean force_refresh,
                                   GCancellable *cancellable, GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  OstreeAwsRuntime *rt = get_runtime ();
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, _ostree_aws_get_credentials_async);

  g_autofree char *key = g_strdup_printf ("%s\n%s", profile ?: "", region);

  g_mutex_lock (&rt->lock);
  ProviderEntry *entry = g_hash_table_lookup (rt->providers, key);
  if (entry != NULL && force_refresh)
    {
      /* The caching provider has no invalidate call, so replace it. The old
       * one is deliberately leaked rather than released: another fetcher may
       * have a query in flight on it, and forced refreshes are rare. */
      g_hash_table_remove (rt->providers, key);
      entry = NULL;
    }
  if (entry == NULL)
    {
      gboolean has_sso = FALSE;
      struct aws_credentials_provider *provider = build_chain (rt, profile, region, &has_sso);
      if (provider != NULL)
        {
          entry = g_new0 (ProviderEntry, 1);
          entry->provider = provider;
          entry->has_sso = has_sso;
          g_hash_table_insert (rt->providers, g_strdup (key), entry);
        }
    }
  struct aws_credentials_provider *provider = entry ? entry->provider : NULL;
  const gboolean has_sso = entry ? entry->has_sso : FALSE;
  g_mutex_unlock (&rt->lock);

  if (provider == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to set up AWS credential providers: %s",
                               aws_error_str (aws_last_error ()));
      return;
    }

  CredsTaskData *data = g_new0 (CredsTaskData, 1);
  data->profile_display = g_strdup (profile ?: (g_getenv ("AWS_PROFILE") ?: "default"));
  data->has_sso = has_sso;
  g_task_set_task_data (task, data, (GDestroyNotify)creds_task_data_free);

  if (aws_credentials_provider_get_credentials (provider, on_get_credentials, g_object_ref (task))
      != AWS_OP_SUCCESS)
    {
      g_object_unref (task); /* the callback will not run */
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to query AWS credentials: %s",
                               aws_error_str (aws_last_error ()));
    }
}

OstreeAwsCredentials *
_ostree_aws_get_credentials_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---- region ---- */

static char *
region_from_profile (OstreeAwsRuntime *rt, const char *profile)
{
  char *ret = NULL;
  struct aws_string *path = aws_get_config_file_path (rt->alloc, NULL);
  if (path == NULL)
    return NULL;
  struct aws_profile_collection *collection
      = aws_profile_collection_new_from_file (rt->alloc, path, AWS_PST_CONFIG);
  aws_string_destroy (path);
  if (collection == NULL)
    return NULL;

  struct aws_byte_cursor override = { 0 };
  if (profile != NULL)
    override = aws_byte_cursor_from_c_str (profile);
  struct aws_string *name = aws_get_profile_name (rt->alloc, profile ? &override : NULL);
  if (name != NULL)
    {
      const struct aws_profile *p = aws_profile_collection_get_profile (collection, name);
      if (p != NULL)
        {
          struct aws_string *prop_name = aws_string_new_from_c_str (rt->alloc, "region");
          const struct aws_profile_property *prop = aws_profile_get_property (p, prop_name);
          if (prop != NULL)
            ret = g_strdup (aws_string_c_str (aws_profile_property_get_value (prop)));
          aws_string_destroy (prop_name);
        }
      aws_string_destroy (name);
    }
  aws_profile_collection_release (collection);
  return ret;
}

typedef struct
{
  gatomicrefcount refcount;
  GMutex lock;
  GCond cond;
  gboolean done;
  char *region;
} ImdsWait;

static void
imds_wait_unref (ImdsWait *wait)
{
  if (!g_atomic_ref_count_dec (&wait->refcount))
    return;
  g_mutex_clear (&wait->lock);
  g_cond_clear (&wait->cond);
  g_free (wait->region);
  g_free (wait);
}

static void
on_imds_instance_info (const struct aws_imds_instance_info *info, int error_code, void *user_data)
{
  ImdsWait *wait = user_data;
  g_mutex_lock (&wait->lock);
  if (error_code == 0 && info != NULL && info->region.len > 0)
    wait->region = g_strndup ((const char *)info->region.ptr, info->region.len);
  wait->done = TRUE;
  g_cond_signal (&wait->cond);
  g_mutex_unlock (&wait->lock);
  imds_wait_unref (wait);
}

static char *
region_from_imds (OstreeAwsRuntime *rt)
{
  if (imds_disabled ())
    return NULL;

  struct aws_imds_client_options opts;
  AWS_ZERO_STRUCT (opts);
  opts.bootstrap = rt->bootstrap;
  opts.imds_version = IMDS_PROTOCOL_V2;
  struct aws_imds_client *client = aws_imds_client_new (rt->alloc, &opts);
  if (client == NULL)
    return NULL;

  char *ret = NULL;
  ImdsWait *wait = g_new0 (ImdsWait, 1);
  g_atomic_ref_count_init (&wait->refcount);
  g_mutex_init (&wait->lock);
  g_cond_init (&wait->cond);

  g_atomic_ref_count_inc (&wait->refcount); /* for the callback */
  if (aws_imds_client_get_instance_info (client, on_imds_instance_info, wait) != AWS_OP_SUCCESS)
    {
      imds_wait_unref (wait); /* the callback will not run */
    }
  else
    {
      const gint64 deadline
          = g_get_monotonic_time () + OSTREE_AWS_IMDS_REGION_TIMEOUT_SECS * G_TIME_SPAN_SECOND;
      g_mutex_lock (&wait->lock);
      while (!wait->done)
        {
          if (!g_cond_wait_until (&wait->cond, &wait->lock, deadline))
            break;
        }
      ret = g_steal_pointer (&wait->region);
      g_mutex_unlock (&wait->lock);
    }
  imds_wait_unref (wait);
  aws_imds_client_release (client);
  return ret;
}

char *
_ostree_aws_resolve_region (const char *url_region, const char *profile, GCancellable *cancellable,
                            GError **error)
{
  if (url_region != NULL)
    return g_strdup (url_region);

  static const char *const env_names[] = { "AWS_REGION", "AWS_DEFAULT_REGION" };
  for (guint i = 0; i < G_N_ELEMENTS (env_names); i++)
    {
      const char *v = g_getenv (env_names[i]);
      if (region_is_valid (v))
        return g_strdup (v);
    }

  OstreeAwsRuntime *rt = get_runtime ();
  g_autofree char *from_profile = region_from_profile (rt, profile);
  if (region_is_valid (from_profile))
    return g_steal_pointer (&from_profile);

  g_autofree char *from_imds = region_from_imds (rt);
  if (region_is_valid (from_imds))
    return g_steal_pointer (&from_imds);

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
               "Could not determine the AWS region for the s3:// remote; add ?region=<region> "
               "to the URL or set AWS_REGION");
  return NULL;
}

/* ---- signing ---- */

typedef struct
{
  GPtrArray *headers;
  gboolean done;
  int error_code;
} SignState;

static void
on_sign_complete (struct aws_signing_result *result, int error_code, void *user_data)
{
  SignState *state = user_data;
  state->done = TRUE;
  state->error_code = error_code;
  if (error_code != 0)
    return;

  struct aws_array_list *list = NULL;
  aws_signing_result_get_property_list (result, g_aws_http_headers_property_list_name, &list);
  const size_t n = list ? aws_array_list_length (list) : 0;
  for (size_t i = 0; i < n; i++)
    {
      struct aws_signing_result_property prop;
      AWS_ZERO_STRUCT (prop);
      if (aws_array_list_get_at (list, &prop, i) != AWS_OP_SUCCESS)
        continue;
      g_ptr_array_add (state->headers, g_strdup_printf ("%s: %s", aws_string_c_str (prop.name),
                                                        aws_string_c_str (prop.value)));
    }
}

gboolean
_ostree_aws_sign_get_at (OstreeAwsCredentials *creds, const char *url, const char *region,
                         guint64 unix_time, GPtrArray *headers, GError **error)
{
  OstreeAwsRuntime *rt = get_runtime ();

  g_autoptr (GUri) uri = g_uri_parse (url, G_URI_FLAGS_ENCODED, error);
  if (uri == NULL)
    return FALSE;
  const char *scheme = g_uri_get_scheme (uri);
  const int port = g_uri_get_port (uri);
  const gboolean default_port = port == -1 || (port == 443 && g_str_equal (scheme, "https"))
                                || (port == 80 && g_str_equal (scheme, "http"));
  g_autofree char *host = default_port ? g_strdup (g_uri_get_host (uri))
                                       : g_strdup_printf ("%s:%d", g_uri_get_host (uri), port);
  const char *path = g_uri_get_path (uri);
  const char *query = g_uri_get_query (uri);
  g_autofree char *path_and_query
      = query ? g_strdup_printf ("%s?%s", *path ? path : "/", query) : g_strdup (*path ? path : "/");

  struct aws_http_message *msg = aws_http_message_new_request (rt->alloc);
  g_assert (msg != NULL);
  aws_http_message_set_request_method (msg, aws_byte_cursor_from_c_str ("GET"));
  aws_http_message_set_request_path (msg, aws_byte_cursor_from_c_str (path_and_query));
  struct aws_http_header host_header
      = { .name = aws_byte_cursor_from_c_str ("host"), .value = aws_byte_cursor_from_c_str (host) };
  aws_http_message_add_header (msg, host_header);
  struct aws_signable *signable = aws_signable_new_http_request (rt->alloc, msg);
  g_assert (signable != NULL);

  struct aws_signing_config_aws config;
  AWS_ZERO_STRUCT (config);
  config.config_type = AWS_SIGNING_CONFIG_AWS;
  config.algorithm = AWS_SIGNING_ALGORITHM_V4;
  config.signature_type = AWS_ST_HTTP_REQUEST_HEADERS;
  config.region = aws_byte_cursor_from_c_str (region);
  config.service = aws_byte_cursor_from_c_str ("s3");
  aws_date_time_init_epoch_secs (&config.date, (double)unix_time);
  /* S3 wants the path signed exactly as sent: encoded once, not normalized. */
  config.flags.use_double_uri_encode = false;
  config.flags.should_normalize_uri_path = false;
  config.signed_body_value = g_aws_signed_body_value_unsigned_payload;
  config.signed_body_header = AWS_SBHT_X_AMZ_CONTENT_SHA256;
  config.credentials = creds->creds;

  SignState state = { .headers = headers };
  const int rc = aws_sign_request_aws (rt->alloc, signable, (struct aws_signing_config_base *)&config,
                                       on_sign_complete, &state);
  gboolean ret = FALSE;
  if (rc != AWS_OP_SUCCESS)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to sign S3 request: %s",
                 aws_error_str (aws_last_error ()));
  else if (!state.done)
    /* With explicit credentials aws-c-auth completes inline. If that ever
     * changes, fail loudly; @state must not be touched after we return. */
    g_error ("aws_sign_request_aws did not complete synchronously");
  else if (state.error_code != 0)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to sign S3 request: %s",
                 aws_error_str (state.error_code));
  else
    ret = TRUE;

  aws_signable_destroy (signable);
  aws_http_message_release (msg);
  return ret;
}

gboolean
_ostree_aws_sign_get (OstreeAwsCredentials *creds, const char *url, const char *region,
                      GPtrArray *headers, GError **error)
{
  return _ostree_aws_sign_get_at (creds, url, region, g_get_real_time () / G_USEC_PER_SEC, headers,
                                  error);
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
nix develop -c bash -c 'make check TESTS=tests/test-fetcher-s3 2>&1 | grep -E "^(PASS|FAIL|ERROR|# (TOTAL|PASS|FAIL))"'
```
Expected: thirteen `PASS:` lines (the nine from Task 3 plus `/aws/sign`, `/aws/sign-port`, `/aws/resolve-region`, `/aws/credentials-chain`), `# FAIL:  0`.

`/aws/credentials-chain` emits GLib debug notes that `setenv()` is not thread-safe. That is expected: the test changes the environment after the AWS thread exists, and nothing else is running.

- [ ] **Step 7: Build the library**

Run:
```bash
nix develop -c bash -c 'make -j"$(nproc)" 2>&1 | grep -E "warning|error" ; echo build-done'
```
Expected: only `build-done`. If the stricter library flags reject something in `ostree-aws.c` that the test build accepted, fix the code, not the flags.

- [ ] **Step 8: Commit**

```bash
git add src/libostree/ostree-aws.h src/libostree/ostree-aws.c tests/test-fetcher-s3.c Makefile-libostree.am Makefile-tests.am
git commit -m "lib/fetcher: add AWS credential chain, region resolution and SigV4 signing

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Accept the `s3` scheme in URI validation

**Files:**
- Modify: `src/libostree/ostree-fetcher-uri.c:138-154` (`_ostree_fetcher_uri_validate`)
- Modify: `tests/test-fetcher-s3.c`
- Modify: `Makefile-tests.am` (the Task 3 block)

**Interfaces:**
- Consumes: `_ostree_s3_location_parse`, `OstreeS3Location` (Task 3).
- Produces: `_ostree_fetcher_uri_validate` returns TRUE for a well-formed `s3://` URI when built with AWS support, and FALSE with the `_ostree_s3_location_parse` error otherwise. Without AWS support it returns FALSE with a message containing `this libostree was built without S3 support`.

- [ ] **Step 1: Write the failing test**

In `tests/test-fetcher-s3.c`, add after `#include "ostree-fetcher-s3.h"`:

```c
#include "libglnx.h"
#include "ostree-fetcher.h"
```

Insert before `int\nmain (int argc, char **argv)`:

```c
static void
test_uri_validate (void)
{
  g_autoptr (GError) error = NULL;

  g_autoptr (OstreeFetcherURI) good
      = _ostree_fetcher_uri_parse ("s3://my-bucket/repo?region=us-east-1", &error);
  g_assert_no_error (error);
  g_assert_true (_ostree_fetcher_uri_validate (good, &error));
  g_assert_no_error (error);

  g_autoptr (OstreeFetcherURI) bad
      = _ostree_fetcher_uri_parse ("s3://my-bucket/repo?regoin=us-east-1", &error);
  g_assert_no_error (error);
  g_assert_false (_ostree_fetcher_uri_validate (bad, &error));
  g_assert_nonnull (strstr (error->message, "Unknown parameter 'regoin'"));
  g_clear_error (&error);

  /* Other schemes behave as before. */
  g_autoptr (OstreeFetcherURI) https = _ostree_fetcher_uri_parse ("https://example.com/repo", &error);
  g_assert_no_error (error);
  g_assert_true (_ostree_fetcher_uri_validate (https, &error));
  g_assert_no_error (error);

  g_autoptr (OstreeFetcherURI) ftp = _ostree_fetcher_uri_parse ("ftp://example.com/repo", &error);
  g_assert_no_error (error);
  g_assert_false (_ostree_fetcher_uri_validate (ftp, &error));
  g_assert_nonnull (strstr (error->message, "Invalid URI scheme"));
  g_clear_error (&error);
}
```

In `main`, add before `return g_test_run ();`:

```c
  g_test_add_func ("/s3/uri-validate", test_uri_validate);
```

In `Makefile-tests.am`, change the source list to:

```make
tests_test_fetcher_s3_SOURCES = \
	src/libostree/ostree-fetcher-s3.c \
	src/libostree/ostree-aws.c \
	src/libostree/ostree-fetcher-uri.c \
	tests/test-fetcher-s3.c
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
nix develop -c bash -c 'make check TESTS=tests/test-fetcher-s3 >/dev/null 2>&1; grep -E "uri-validate|Invalid URI scheme" tests/test-fetcher-s3.log | head -5'
```
Expected: FAIL. The log shows `/s3/uri-validate` failing its first `g_assert_true`, because validation rejects `s3` with `Invalid URI scheme`.

- [ ] **Step 3: Implement**

In `src/libostree/ostree-fetcher-uri.c`, add after `#include "libglnx.h"`:

```c

#ifdef HAVE_AWS
#include "ostree-fetcher-s3.h"
#endif
```

Replace the whole of `_ostree_fetcher_uri_validate` and its comment with:

```c
/* Only accept http, https, file and (when built with AWS support) s3;
 * particularly curl has a ton of other backends like sftp that we don't
 * want, and this also gracefully filters out invalid input.
 */
gboolean
_ostree_fetcher_uri_validate (OstreeFetcherURI *uri, GError **error)
{
  const char *scheme = g_uri_get_scheme ((GUri *)uri);
  if (g_str_equal (scheme, "s3"))
    {
#ifdef HAVE_AWS
      g_autoptr (OstreeS3Location) loc = _ostree_s3_location_parse ((GUri *)uri, error);
      return loc != NULL;
#else
      g_autofree char *s = _ostree_fetcher_uri_to_string (uri);
      return glnx_throw (error, "Invalid URI %s: this libostree was built without S3 support", s);
#endif
    }
  // TODO only allow file if explicitly requested by a higher level
  if (!(g_str_equal (scheme, "http") || g_str_equal (scheme, "https")
        || g_str_equal (scheme, "file")))
    {
      g_autofree char *s = _ostree_fetcher_uri_to_string (uri);
      return glnx_throw (error, "Invalid URI scheme in %s", s);
    }
  return TRUE;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:
```bash
nix develop -c bash -c 'make check TESTS=tests/test-fetcher-s3 2>&1 | grep -E "^(PASS|FAIL|ERROR|# (TOTAL|PASS|FAIL))"'
```
Expected: fourteen `PASS:` lines including `/s3/uri-validate`, `# FAIL:  0`.

- [ ] **Step 5: Commit**

```bash
git add src/libostree/ostree-fetcher-uri.c tests/test-fetcher-s3.c Makefile-tests.am
git commit -m "lib/fetcher: accept and validate s3:// URIs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: End-to-end test against MinIO (written first, fails until Task 8)

**Files:**
- Create: `tests/test-pull-s3.sh` (mode 0755)
- Modify: `Makefile-tests.am` (after `_installed_or_uninstalled_test_scripts += $(libsoup_tests)`, around line 215)

**Interfaces:**
- Consumes: `aws` feature string (Task 2); `minio`, `mc`, `curl`, `python3` from the dev shell.
- Produces: `tests/test-pull-s3.sh`, a 7-assertion TAP test. Tasks 7 and 8 are done when it passes.

- [ ] **Step 1: Write `tests/test-pull-s3.sh`**

```bash
#!/bin/bash
#
# SPDX-License-Identifier: LGPL-2.0+
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

# End-to-end pulls from s3:// remotes against a local MinIO server.
# MinIO is used because it verifies SigV4 signatures and issues temporary
# (session-token) credentials through its STS endpoint.

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_ostree_feature aws
for tool in minio mc curl python3; do
    if ! command -v ${tool} >/dev/null; then
        skip "${tool} is required"
    fi
done
if ! curl --help all | grep -q -e '--aws-sigv4'; then
    skip "curl with --aws-sigv4 is required to request temporary credentials"
fi

echo '1..7'

cd ${test_tmpdir}

# Keep the host's AWS configuration out of the test.
unset AWS_PROFILE AWS_REGION AWS_DEFAULT_REGION AWS_ACCESS_KEY_ID \
      AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN AWS_ROLE_ARN \
      AWS_WEB_IDENTITY_TOKEN_FILE AWS_CONTAINER_CREDENTIALS_FULL_URI \
      AWS_CONTAINER_CREDENTIALS_RELATIVE_URI
export AWS_EC2_METADATA_DISABLED=true
export AWS_CONFIG_FILE=${test_tmpdir}/aws-config-absent
export AWS_SHARED_CREDENTIALS_FILE=${test_tmpdir}/aws-credentials-absent

# --- Server-side repo: ~200 objects, with a from-scratch static delta whose
# --- path contains '+', which S3 only serves when it is percent-encoded.
mkdir -p files/sub
for i in $(seq 1 200); do
    echo "content ${i}" > files/sub/file-${i}
done
ostree_repo_init srv --mode=archive
found_plus=false
for attempt in $(seq 1 60); do
    echo "attempt ${attempt}" > files/marker
    ${CMD_PREFIX} ostree --repo=srv commit -b main -s "commit ${attempt}" --tree=dir=files >/dev/null
    rm -rf srv/deltas
    ${CMD_PREFIX} ostree --repo=srv static-delta generate --empty main >/dev/null
    if find srv/deltas -path '*+*' | grep -q .; then
        found_plus=true
        break
    fi
done
${found_plus} || fatal "could not produce a static delta path containing '+'"
${CMD_PREFIX} ostree --repo=srv summary -u
srv_rev=$(${CMD_PREFIX} ostree --repo=srv rev-parse main)

# --- MinIO
root_user=ostreetestroot
root_secret=ostreetestrootsecret
port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
endpoint=http://127.0.0.1:${port}
mkdir minio-data
MINIO_ROOT_USER=${root_user} MINIO_ROOT_PASSWORD=${root_secret} \
    minio server --quiet --address 127.0.0.1:${port} ${test_tmpdir}/minio-data > minio.log 2>&1 &
minio_pid=$!
kill_minio () {
    kill ${minio_pid} 2>/dev/null || true
}
libtest_exit_cmds+=(kill_minio)
for i in $(seq 1 100); do
    if curl -sf ${endpoint}/minio/health/live >/dev/null; then
        break
    fi
    sleep 0.1
done
curl -sf ${endpoint}/minio/health/live >/dev/null || fatal "MinIO did not start; see minio.log"

export MC_CONFIG_DIR=${test_tmpdir}/mc-config
mc --quiet alias set t ${endpoint} ${root_user} ${root_secret} >/dev/null
mc --quiet mb t/ostree-test >/dev/null
mc --quiet cp --recursive srv/ t/ostree-test/repo/ >/dev/null

s3url="s3://ostree-test/repo?endpoint=${endpoint}&region=us-east-1"

# Run ostree with the given access key and secret, and optionally a session token.
ostree_as () {
    local key=$1 secret=$2 token=$3
    shift 3
    if test -n "${token}"; then
        AWS_ACCESS_KEY_ID=${key} AWS_SECRET_ACCESS_KEY=${secret} AWS_SESSION_TOKEN=${token} \
            ${CMD_PREFIX} ostree "$@"
    else
        AWS_ACCESS_KEY_ID=${key} AWS_SECRET_ACCESS_KEY=${secret} ${CMD_PREFIX} ostree "$@"
    fi
}

new_client_repo () {
    rm -rf $1
    ostree_repo_init $1
    ${CMD_PREFIX} ostree --repo=$1 remote add --set=gpg-verify=false origin "$2"
}

# 1: static credentials from the environment
new_client_repo repo1 "${s3url}"
ostree_as ${root_user} ${root_secret} "" --repo=repo1 pull --disable-static-deltas origin main
assert_streq "$(${CMD_PREFIX} ostree --repo=repo1 rev-parse main)" "${srv_rev}"
${CMD_PREFIX} ostree --repo=repo1 fsck >/dev/null
echo "ok pull with static credentials"

# 2: temporary credentials carrying a session token
mc --quiet admin user add t ostreetempuser ostreetempsecret >/dev/null
mc --quiet admin policy attach t readwrite --user ostreetempuser >/dev/null
sts_xml=$(curl -sS --aws-sigv4 "aws:amz:us-east-1:sts" --user ostreetempuser:ostreetempsecret \
               -d "Action=AssumeRole&Version=2011-06-15&DurationSeconds=900" ${endpoint}/)
xml_field () {
    echo "${sts_xml}" | sed -n "s:.*<$1>\([^<]*\)</$1>.*:\1:p"
}
temp_key=$(xml_field AccessKeyId)
temp_secret=$(xml_field SecretAccessKey)
temp_token=$(xml_field SessionToken)
test -n "${temp_key}" -a -n "${temp_secret}" -a -n "${temp_token}" \
    || fatal "MinIO STS did not return temporary credentials: ${sts_xml}"
new_client_repo repo2 "${s3url}"
ostree_as ${temp_key} ${temp_secret} ${temp_token} --repo=repo2 pull --disable-static-deltas origin main
assert_streq "$(${CMD_PREFIX} ostree --repo=repo2 rev-parse main)" "${srv_rev}"
# The same key and secret without the token must be refused, proving the token is sent and signed.
new_client_repo repo2b "${s3url}"
if ostree_as ${temp_key} ${temp_secret} "" --repo=repo2b pull origin main 2>err.txt; then
    fatal "pulled with temporary credentials but no session token"
fi
echo "ok pull with temporary credentials"

# 3: static deltas, whose path contains '+'
new_client_repo repo3 "${s3url}"
ostree_as ${root_user} ${root_secret} "" --repo=repo3 pull --require-static-deltas origin main
assert_streq "$(${CMD_PREFIX} ostree --repo=repo3 rev-parse main)" "${srv_rev}"
echo "ok pull with static deltas"

# 4: a wrong secret is a loud, identifiable failure
new_client_repo repo4 "${s3url}"
if ostree_as ${root_user} wrong-secret "" --repo=repo4 pull origin main 2>err.txt; then
    fatal "pulled with a wrong secret key"
fi
assert_file_has_content err.txt "SignatureDoesNotMatch"
echo "ok wrong credentials rejected"

# 5: a missing ref is "not found", not an authorization error
new_client_repo repo5 "${s3url}"
if ostree_as ${root_user} ${root_secret} "" --repo=repo5 pull origin nosuchref 2>err.txt; then
    fatal "pulled a ref that does not exist"
fi
assert_file_has_content err.txt "nosuchref"
assert_not_file_has_content err.txt "AccessDenied"
assert_not_file_has_content err.txt "SignatureDoesNotMatch"
echo "ok missing ref"

# 6: an unknown URL parameter is rejected instead of silently ignored
new_client_repo repo6 "s3://ostree-test/repo?endpoint=${endpoint}&regoin=us-east-1"
if ostree_as ${root_user} ${root_secret} "" --repo=repo6 pull origin main 2>err.txt; then
    fatal "pulled with an unknown URL parameter"
fi
assert_file_has_content err.txt "Unknown parameter 'regoin'"
echo "ok unknown parameter rejected"

# 7: --max-outstanding-fetcher-requests controls how many connections are opened
count_connections () {
    new_client_repo repo7 "${s3url}"
    OSTREE_DEBUG_HTTP=1 ostree_as ${root_user} ${root_secret} "" --repo=repo7 pull \
        --disable-static-deltas --max-outstanding-fetcher-requests=$1 origin main 2>debug.txt >/dev/null
    grep -c 'Connected to ' debug.txt || true
}
serial=$(count_connections 1)
parallel=$(count_connections 16)
# One connection per fetcher instance; a pull creates at most three.
test "${serial}" -ge 1 -a "${serial}" -le 3 \
    || fatal "expected 1-3 connections at concurrency 1, got ${serial}"
test "${parallel}" -gt "${serial}" \
    || fatal "expected more connections at concurrency 16 (${parallel}) than at 1 (${serial})"
test "${parallel}" -le 48 \
    || fatal "expected at most 16 connections per fetcher at concurrency 16, got ${parallel}"
echo "ok connection count follows the concurrency limit"
```

Then:
```bash
chmod +x tests/test-pull-s3.sh
```

- [ ] **Step 2: Confirm the helpers the script relies on exist**

Run:
```bash
grep -n '^assert_streq\|^assert_not_file_has_content\|^libtest_exit_cmds\|^ostree_repo_init\|^skip_without_ostree_feature' tests/libtest.sh tests/libtest-core.sh
```
Expected: `assert_streq` and `assert_not_file_has_content` in `tests/libtest-core.sh`; `libtest_exit_cmds`, `ostree_repo_init` and `skip_without_ostree_feature` in `tests/libtest.sh`. `ostree_repo_init <dir> [init args]` passes extra arguments to `ostree init`, and `libtest_exit_cmds` takes command names, which is why the script registers the `kill_minio` function.

- [ ] **Step 3: Register the test**

In `Makefile-tests.am`, find:

```make
libsoup_tests = tests/test-remote-cookies.sh
if USE_LIBSOUP_OR_LIBSOUP3
_installed_or_uninstalled_test_scripts += $(libsoup_tests)
else
EXTRA_DIST += $(libsoup_tests)
endif
```

Insert immediately after that `endif`:

```make

if USE_AWS
_installed_or_uninstalled_test_scripts += tests/test-pull-s3.sh
else
EXTRA_DIST += tests/test-pull-s3.sh
endif
```

- [ ] **Step 4: Run it to verify it fails for the right reason**

Run:
```bash
nix develop -c bash -c 'make check TESTS=tests/test-pull-s3.sh >/dev/null 2>&1; tail -25 tests/test-pull-s3.sh.log'
```
Expected: FAIL at the first pull. MinIO starts and the upload succeeds; then `ostree pull` dies inside the curl backend (an abort on the `g_str_has_prefix (eff_url, "http")` assertion, or a curl "protocol not supported" error), because the backend does not know `s3://` yet.

If it fails earlier (MinIO not starting, `mc` errors, STS not answering), fix the script now: those parts do not depend on later tasks. In particular, if `mc admin policy attach` is rejected by this `mc` version, use `mc admin policy set t readwrite user=ostreetempuser`.

- [ ] **Step 5: Commit**

```bash
git add tests/test-pull-s3.sh Makefile-tests.am
git commit -m "tests: add s3:// pull test against a local MinIO server

The test fails until the curl backend learns s3://.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Curl backend wiring

**Files:**
- Modify: `src/libostree/ostree-fetcher-curl.c`

**Interfaces:**
- Consumes: everything produced by Tasks 3 and 4.
- Produces: no new symbols. Behaviour: `_ostree_fetcher_request_to_tmpfile` / `_to_membuf` accept `s3://` mirrorlist entries. `_ostree_fetcher_set_max_outstanding_fetcher_requests` also sets curl's total-connection cap to `MAX (8, n)`.

Line numbers below refer to the file at `fdef7e6`; each edit also quotes the text to find. All S3 code is inside `#ifdef HAVE_AWS`.

How the existing code works, for orientation:
- A request is a `GTask` whose task data is a `FetcherRequest`. `self->outstanding_requests` holds the only long-lived reference to each task.
- `initiate_next_curl_request (req, task)` builds a curl easy handle for mirror `req->idx` and adds it to the multi handle. It is called for the first attempt and again for each mirror failover.
- `check_multi_info` runs when transfers finish; it completes the task, then removes it from `outstanding_requests`, and drops `self->mainctx` when the set becomes empty.

- [ ] **Step 1: Add includes and constants**

Find `#include "otutil.h"` (the last include) and add after it:

```c

#ifdef HAVE_AWS
#include "ostree-aws.h"
#include "ostree-fetcher-s3.h"

/* How much of a non-2xx S3 response body to keep for error reporting. */
#define S3_ERROR_BODY_MAX (64 * 1024)
/* Signing region for ?endpoint= remotes that resolve none; S3-compatible
 * servers generally accept any. */
#define S3_ENDPOINT_FALLBACK_REGION "us-east-1"
#endif
```

- [ ] **Step 2: Add fetcher and request state**

In `struct OstreeFetcher`, find `guint64 bytes_transferred;` and add after it:

```c

#ifdef HAVE_AWS
  GHashTable *s3_remotes; /* base URI string -> S3Remote* */
  GHashTable *s3_creds;   /* "profile\nregion" -> S3CredState* */
#endif
```

Near the top of the file, find the forward declaration `static void destroy_and_unref_source (GSource *source);` and add after it:

```c

#ifdef HAVE_AWS
typedef struct S3Remote S3Remote;
typedef struct S3CredState S3CredState;
static void s3_remote_free (S3Remote *remote);
static void s3_cred_state_free (S3CredState *cs);
#endif
```

In `struct FetcherRequest`, find `OstreeFetcher *fetcher;` and add after it:

```c

#ifdef HAVE_AWS
  gboolean is_s3;                 /* current attempt targets an s3:// mirror */
  gboolean s3_retried_expired;    /* already retried once after an expired-token error */
  GString *s3_error_body;         /* body of a non-2xx response, capped */
  char *s3_bucket_region;         /* x-amz-bucket-region response header */
  const char *s3_creds_key;       /* owned by the S3Remote */
  OstreeAwsCredentials *s3_creds; /* what the current attempt was signed with */
#endif
```

- [ ] **Step 3: Add the S3 helpers**

Find the function `check_multi_info` (its comment is `/* Check for completed transfers, and remove their easy handles */`). Insert this whole block immediately before that comment:

```c
#ifdef HAVE_AWS
/* One s3:// base URI as configured on a remote. */
struct S3Remote
{
  OstreeS3Location *loc;
  char *region;    /* resolved signing region */
  char *creds_key; /* key into OstreeFetcher.s3_creds */
};

/* Credentials for one (profile, region), shared by every request using it. */
struct S3CredState
{
  OstreeAwsCredentials *creds; /* NULL until fetched, or after being dropped as expired */
  guint64 fetched_at;          /* wall-clock seconds */
  gboolean fetching;           /* a fetch is in flight */
  gboolean force_next;         /* the next fetch bypasses the process-wide cache */
  GQueue waiting;              /* GTask*, borrowed: outstanding_requests owns them */
};

static void
s3_remote_free (S3Remote *remote)
{
  _ostree_s3_location_free (remote->loc);
  g_free (remote->region);
  g_free (remote->creds_key);
  g_free (remote);
}

static void
s3_cred_state_free (S3CredState *cs)
{
  g_clear_pointer (&cs->creds, _ostree_aws_credentials_unref);
  g_queue_clear (&cs->waiting);
  g_free (cs);
}

static guint64
s3_now (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

/* Parse @baseuri and resolve its region, once per fetcher. */
static S3Remote *
s3_get_remote (OstreeFetcher *self, GUri *baseuri, GError **error)
{
  g_autofree char *key = g_uri_to_string_partial (baseuri, G_URI_HIDE_PASSWORD);
  S3Remote *remote = g_hash_table_lookup (self->s3_remotes, key);
  if (remote != NULL)
    return remote;

  g_autoptr (OstreeS3Location) loc = _ostree_s3_location_parse (baseuri, error);
  if (loc == NULL)
    return NULL;

  g_autoptr (GError) region_error = NULL;
  g_autofree char *region
      = _ostree_aws_resolve_region (loc->region, loc->profile, NULL, &region_error);
  if (region == NULL)
    {
      if (loc->endpoint == NULL)
        {
          g_propagate_error (error, g_steal_pointer (&region_error));
          return NULL;
        }
      region = g_strdup (S3_ENDPOINT_FALLBACK_REGION);
    }

  remote = g_new0 (S3Remote, 1);
  remote->creds_key = g_strdup_printf ("%s\n%s", loc->profile ?: "", region);
  remote->region = g_steal_pointer (&region);
  remote->loc = g_steal_pointer (&loc);
  g_hash_table_insert (self->s3_remotes, g_steal_pointer (&key), remote);
  return remote;
}

typedef struct
{
  GTask *task; /* owned */
  GError *error;
} S3FailData;

static void
s3_fail_data_free (gpointer user_data)
{
  S3FailData *data = user_data;
  g_clear_error (&data->error);
  g_object_unref (data->task);
  g_free (data);
}

static gboolean
s3_fail_idle_cb (gpointer user_data)
{
  S3FailData *data = user_data;
  OstreeFetcher *self = g_task_get_source_object (data->task);

  g_task_return_error (data->task, g_steal_pointer (&data->error));
  /* Same bookkeeping as the end of check_multi_info(). */
  g_hash_table_remove (self->outstanding_requests, data->task);
  if (g_hash_table_size (self->outstanding_requests) == 0)
    g_clear_pointer (&self->mainctx, g_main_context_unref);
  return G_SOURCE_REMOVE;
}

/* Fail a request that never reached curl. This is deferred to an idle so
 * it behaves the same whether we were called from
 * _ostree_fetcher_request_async() (which adds the task to
 * outstanding_requests only after we return) or from a callback.
 * Takes ownership of @error. */
static void
s3_fail_request (OstreeFetcher *self, GTask *task, GError *error)
{
  S3FailData *data = g_new0 (S3FailData, 1);
  data->task = g_object_ref (task);
  data->error = error;

  GSource *source = g_idle_source_new ();
  g_source_set_callback (source, s3_fail_idle_cb, data, s3_fail_data_free);
  g_source_attach (source, self->mainctx);
  g_source_unref (source);
}

typedef struct
{
  OstreeFetcher *self; /* owned */
  char *creds_key;
} S3CredsFetch;

static void
s3_on_credentials (GObject *source, GAsyncResult *result, gpointer user_data)
{
  S3CredsFetch *fetch = user_data;
  OstreeFetcher *self = fetch->self;
  S3CredState *cs = g_hash_table_lookup (self->s3_creds, fetch->creds_key);
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeAwsCredentials) creds = _ostree_aws_get_credentials_finish (result, &error);
  const guint64 now = s3_now ();

  g_assert (cs != NULL);
  cs->fetching = FALSE;

  /* Credentials that are already expired would only be refetched again
   * immediately; turn that into an error instead of a loop. */
  if (creds != NULL && _ostree_aws_credentials_get_expiry (creds) <= now)
    {
      g_clear_pointer (&creds, _ostree_aws_credentials_unref);
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   "AWS credential provider returned credentials that have already expired");
    }
  if (creds != NULL)
    {
      g_clear_pointer (&cs->creds, _ostree_aws_credentials_unref);
      cs->creds = g_steal_pointer (&creds);
      cs->fetched_at = now;
    }

  /* Take the list first: resuming a request can park it again. */
  GQueue waiting = cs->waiting;
  g_queue_init (&cs->waiting);
  GTask *task;
  while ((task = g_queue_pop_head (&waiting)) != NULL)
    {
      if (error == NULL)
        initiate_next_curl_request (g_task_get_task_data (task), task);
      else
        s3_fail_request (self, task, g_error_copy (error));
    }

  g_object_unref (fetch->self);
  g_free (fetch->creds_key);
  g_free (fetch);
}

static void
s3_start_credentials_fetch (OstreeFetcher *self, S3Remote *remote, S3CredState *cs)
{
  S3CredsFetch *fetch = g_new0 (S3CredsFetch, 1);
  fetch->self = g_object_ref (self);
  fetch->creds_key = g_strdup (remote->creds_key);

  /* Replacing credentials we already hold means they are stale; the
   * process-wide cache would just hand the same ones back. */
  const gboolean force = cs->force_next || cs->creds != NULL;
  cs->force_next = FALSE;
  cs->fetching = TRUE;

  /* The result is delivered on the thread-default context; make it ours. */
  g_main_context_push_thread_default (self->mainctx);
  _ostree_aws_get_credentials_async (remote->loc->profile, remote->region, force, NULL,
                                     s3_on_credentials, fetch);
  g_main_context_pop_thread_default (self->mainctx);
}

/* Prepare one attempt at an s3:// mirror.
 *
 * On success either *out_parked is TRUE (the request waits for
 * credentials and will be re-initiated), or *out_url is the URL to
 * request and the signed headers have been appended to req->req_headers.
 */
static gboolean
s3_prepare_request (OstreeFetcher *self, FetcherRequest *req, GTask *task, GUri *baseuri,
                    char **out_url, gboolean *out_parked, GError **error)
{
  S3Remote *remote = s3_get_remote (self, baseuri, error);
  if (remote == NULL)
    return FALSE;

  S3CredState *cs = g_hash_table_lookup (self->s3_creds, remote->creds_key);
  if (cs == NULL)
    {
      cs = g_new0 (S3CredState, 1);
      g_queue_init (&cs->waiting);
      g_hash_table_insert (self->s3_creds, g_strdup (remote->creds_key), cs);
    }

  if (cs->creds == NULL
      || !_ostree_s3_credentials_usable (_ostree_aws_credentials_get_expiry (cs->creds),
                                         cs->fetched_at, s3_now ()))
    {
      g_queue_push_tail (&cs->waiting, task);
      if (!cs->fetching)
        s3_start_credentials_fetch (self, remote, cs);
      *out_parked = TRUE;
      return TRUE;
    }

  g_autofree char *url
      = _ostree_s3_location_to_request_url (remote->loc, remote->region, req->filename);
  g_autoptr (GPtrArray) headers = g_ptr_array_new_with_free_func (g_free);
  if (!_ostree_aws_sign_get (cs->creds, url, remote->region, headers, error))
    return FALSE;
  for (guint i = 0; i < headers->len; i++)
    req->req_headers = curl_slist_append (req->req_headers, headers->pdata[i]);

  req->s3_creds_key = remote->creds_key;
  req->s3_creds = _ostree_aws_credentials_ref (cs->creds);
  *out_url = g_steal_pointer (&url);
  return TRUE;
}

/* Handle a non-2xx, non-304 response to an s3:// request. Exactly one of
 * these happens: *out_retry is set (re-sign and retry the same mirror),
 * *out_next_mirror is set, or @task is completed with an error. */
static void
s3_complete_error (OstreeFetcher *self, FetcherRequest *req, GTask *task, const char *eff_url,
                   long response, gboolean retry_all, gboolean *out_next_mirror,
                   gboolean *out_retry)
{
  g_autofree char *code = NULL;
  g_autofree char *message = NULL;
  g_autofree char *body_region = NULL;
  if (req->s3_error_body != NULL)
    (void)_ostree_s3_parse_error_body (req->s3_error_body->str, req->s3_error_body->len, &code,
                                       &message, &body_region);

  /* S3 reports ExpiredToken with HTTP 400 and RequestExpired with 403, so
   * go by the error code, not the status. */
  const gboolean expired
      = code != NULL
        && (g_str_equal (code, "ExpiredToken") || g_str_equal (code, "TokenRefreshRequired")
            || g_str_equal (code, "RequestExpired"));
  if (expired && !req->s3_retried_expired)
    {
      S3CredState *cs = g_hash_table_lookup (self->s3_creds, req->s3_creds_key);
      /* Only drop the credentials this request was signed with: when many
       * requests fail together, the first one may already have replaced them. */
      if (cs != NULL && cs->creds == req->s3_creds)
        {
          g_clear_pointer (&cs->creds, _ostree_aws_credentials_unref);
          cs->force_next = TRUE;
        }
      req->s3_retried_expired = TRUE;
      *out_retry = TRUE;
      return;
    }

  if (req->idx + 1 < req->mirrorlist->len)
    {
      *out_next_mirror = TRUE;
      return;
    }

  const char *bucket_region = req->s3_bucket_region ?: body_region;
  const gboolean wrong_region
      = response == 301 || response == 307
        || (code != NULL && g_str_equal (code, "AuthorizationHeaderMalformed")
            && body_region != NULL);
  GIOErrorEnum giocode;
  g_autofree char *msg = NULL;

  if (response == 404)
    {
      giocode = G_IO_ERROR_NOT_FOUND;
      msg = g_strdup_printf ("While fetching %s: Server returned HTTP %ld", eff_url, response);
    }
  else if (wrong_region)
    {
      giocode = G_IO_ERROR_FAILED;
      if (bucket_region != NULL)
        msg = g_strdup_printf ("While fetching %s: bucket is in region %s; add ?region=%s to "
                               "the s3:// URL",
                               eff_url, bucket_region, bucket_region);
      else
        msg = g_strdup_printf ("While fetching %s: bucket is in a different region (HTTP %ld); "
                               "set ?region= on the s3:// URL",
                               eff_url, response);
    }
  else if (response == 403 || expired)
    {
      giocode = G_IO_ERROR_PERMISSION_DENIED;
      msg = g_strdup_printf ("While fetching %s: S3 returned HTTP %ld %s: %s", eff_url, response,
                             code ?: "(no error code)", message ?: "(no message)");
    }
  else
    {
      giocode = _ostree_fetcher_http_status_code_to_io_error (response, retry_all);
      if (code != NULL)
        msg = g_strdup_printf ("While fetching %s: S3 returned HTTP %ld %s: %s", eff_url, response,
                               code, message ?: "(no message)");
      else
        msg = g_strdup_printf ("While fetching %s: Server returned HTTP %ld", eff_url, response);
    }

  g_task_return_new_error (task, G_IO_ERROR, giocode, "%s", msg);
  if (self->remote_name
      && !((req->flags & OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT) > 0
           && giocode == G_IO_ERROR_NOT_FOUND))
    _ostree_fetcher_journal_failure (self->remote_name, eff_url, msg);
}
#endif /* HAVE_AWS */

```

- [ ] **Step 4: Create and destroy the tables**

In `_ostree_fetcher_init`, find:

```c
  self->sockets = g_hash_table_new_full (NULL, NULL, (GDestroyNotify)sock_unref, NULL);
```

Add after it:

```c
#ifdef HAVE_AWS
  self->s3_remotes
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)s3_remote_free);
  self->s3_creds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)s3_cred_state_free);
#endif
```

In `_ostree_fetcher_finalize`, find:

```c
  g_hash_table_unref (self->sockets);
```

Add after it:

```c
#ifdef HAVE_AWS
  g_hash_table_unref (self->s3_remotes);
  g_hash_table_unref (self->s3_creds);
#endif
```

In `request_unref`, find:

```c
  g_clear_pointer (&req->req_headers, curl_slist_free_all);
  curl_easy_cleanup (req->easy);
```

Add after those two lines:

```c
#ifdef HAVE_AWS
  if (req->s3_error_body)
    g_string_free (req->s3_error_body, TRUE);
  g_free (req->s3_bucket_region);
  g_clear_pointer (&req->s3_creds, _ostree_aws_credentials_unref);
#endif
```

- [ ] **Step 5: Capture S3 error bodies and the bucket-region header**

In `write_cb`, find:

```c
  if (req->caught_write_error)
    return -1;
```

Add after it:

```c

#ifdef HAVE_AWS
  if (req->is_s3)
    {
      long response = 0;
      (void)curl_easy_getinfo (req->easy, CURLINFO_RESPONSE_CODE, &response);
      if (!(response >= 200 && response < 300))
        {
          /* An error document, not content: keep a bounded copy for the
           * error message and never let it reach the output. */
          if (req->s3_error_body == NULL)
            req->s3_error_body = g_string_new (NULL);
          if (req->s3_error_body->len < S3_ERROR_BODY_MAX)
            g_string_append_len (req->s3_error_body, ptr,
                                 MIN (realsize, S3_ERROR_BODY_MAX - req->s3_error_body->len));
          return realsize;
        }
    }
#endif
```

In `response_header_cb`, find the closing brace of the `else if (g_ascii_strncasecmp (line, last_modified_header, …` block (just before `return real_size;`) and add after that brace:

```c
#ifdef HAVE_AWS
  else if (req->is_s3 && g_ascii_strncasecmp (line, "x-amz-bucket-region:", 20) == 0)
    {
      g_free (req->s3_bucket_region);
      req->s3_bucket_region = g_strdup (g_strstrip (line + 20));
    }
#endif
```

- [ ] **Step 6: Route S3 error responses in `check_multi_info`**

Find the declaration:

```c
      gboolean continued_request = FALSE;
```

Add after it:

```c
      gboolean s3_retry = FALSE;
```

Find:

```c
          if (!is_file && !(response >= 200 && response < 300) && response != 304)
            {
              GIOErrorEnum giocode
                  = _ostree_fetcher_http_status_code_to_io_error (response, retry_all);
```

Replace the first of those lines (the `if (…)` line only) with:

```c
#ifdef HAVE_AWS
          const gboolean s3_failed
              = req->is_s3 && !(response >= 200 && response < 300) && response != 304;
#else
          const gboolean s3_failed = FALSE;
#endif
          if (s3_failed)
            {
#ifdef HAVE_AWS
              s3_complete_error (fetcher, req, task, eff_url, response, retry_all,
                                 &continued_request, &s3_retry);
#endif
            }
          else if (!is_file && !(response >= 200 && response < 300) && response != 304)
```

Find, near the end of the loop body:

```c
      if (continued_request)
        {
          req->idx++;
          initiate_next_curl_request (req, task);
        }
      else
```

Replace with:

```c
      if (continued_request)
        {
          req->idx++;
          initiate_next_curl_request (req, task);
        }
      else if (s3_retry)
        {
          initiate_next_curl_request (req, task);
        }
      else
```

- [ ] **Step 7: Prepare S3 requests in `initiate_next_curl_request`**

Find the start of the function body:

```c
  CURLcode rc;
  OstreeFetcher *self = req->fetcher;

  if (req->easy)
    curl_easy_cleanup (req->easy);
  req->easy = curl_easy_init ();
  g_assert (req->easy);

  g_assert_cmpint (req->idx, <, req->mirrorlist->len);

  GUri *baseuri = req->mirrorlist->pdata[req->idx];
  {
    g_autofree char *uri = request_get_uri (req, baseuri);
    rc = curl_easy_setopt (req->easy, CURLOPT_URL, uri);
    g_assert_cmpint (rc, ==, CURLM_OK);
  }
```

Replace it with:

```c
  CURLcode rc;
  OstreeFetcher *self = req->fetcher;

  /* The old handle must go before anything it points at is freed below. */
  g_clear_pointer (&req->easy, curl_easy_cleanup);

  g_assert_cmpint (req->idx, <, req->mirrorlist->len);

  GUri *baseuri = req->mirrorlist->pdata[req->idx];
  g_autofree char *s3_url = NULL;

#ifdef HAVE_AWS
  req->is_s3 = g_str_equal (g_uri_get_scheme (baseuri), "s3");
  if (req->is_s3)
    {
      g_autoptr (GError) s3_error = NULL;
      gboolean parked = FALSE;

      /* Every attempt is signed afresh; never resend a stale signature. */
      g_clear_pointer (&req->req_headers, curl_slist_free_all);
      g_clear_pointer (&req->s3_creds, _ostree_aws_credentials_unref);
      g_clear_pointer (&req->s3_bucket_region, g_free);
      if (req->s3_error_body)
        g_string_truncate (req->s3_error_body, 0);

      if (!s3_prepare_request (self, req, task, baseuri, &s3_url, &parked, &s3_error))
        {
          s3_fail_request (self, task, g_steal_pointer (&s3_error));
          return;
        }
      if (parked)
        return;
    }
#endif

  req->easy = curl_easy_init ();
  g_assert (req->easy);

  {
    g_autofree char *uri = s3_url ? g_strdup (s3_url) : request_get_uri (req, baseuri);
    rc = curl_easy_setopt (req->easy, CURLOPT_URL, uri);
    g_assert_cmpint (rc, ==, CURLM_OK);
  }
```

Further down in the same function, find:

```c
  rc = curl_easy_setopt (req->easy, CURLOPT_FOLLOWLOCATION, 1L);
  g_assert_cmpint (rc, ==, CURLM_OK);
```

Add after it:

```c
#ifdef HAVE_AWS
  if (req->is_s3)
    {
      /* curl forwards custom headers to redirect targets, which would hand
       * the signature and session token to another host. An S3 redirect
       * means "wrong region"; s3_complete_error() reports it. */
      rc = curl_easy_setopt (req->easy, CURLOPT_FOLLOWLOCATION, 0L);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }
#endif
```

- [ ] **Step 8: Tie the connection cap to the request limit**

Replace the body of `_ostree_fetcher_set_max_outstanding_fetcher_requests` so the function reads:

```c
void
_ostree_fetcher_set_max_outstanding_fetcher_requests (OstreeFetcher *self,
                                                      guint32 opt_max_outstanding_fetcher_requests)
{
  self->opt_max_outstanding_fetcher_requests = opt_max_outstanding_fetcher_requests;
#if CURL_AT_LEAST_VERSION(7, 30, 0)
  /* Servers that only speak HTTP/1.1 (S3 among them) get their concurrency
   * from parallel connections, so don't cap those below the request limit. */
  CURLMcode rc = curl_multi_setopt (self->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                                    (long)MAX (8, opt_max_outstanding_fetcher_requests));
  g_assert_cmpint (rc, ==, CURLM_OK);
#endif
}
```

- [ ] **Step 9: Build**

Run:
```bash
nix develop -c bash -c 'make -j"$(nproc)" 2>&1 | grep -E "warning|error"; echo build-done'
```
Expected: only `build-done`.

- [ ] **Step 10: Run the end-to-end test**

Run:
```bash
nix develop -c bash -c 'make check TESTS=tests/test-pull-s3.sh >/dev/null 2>&1; grep -E "^(ok|not ok|FAIL|ERROR)|fatal|Assertion" tests/test-pull-s3.sh.log'
```
Expected: assertions 1 through 6 print `ok`:
```
ok pull with static credentials
ok pull with temporary credentials
ok pull with static deltas
ok wrong credentials rejected
ok missing ref
ok unknown parameter rejected
```
Assertion 7 is expected to fail here with `expected 1-3 connections at concurrency 1`, because upstream ignores `--max-outstanding-fetcher-requests` until Task 8.

If an earlier assertion fails, read the whole of `tests/test-pull-s3.sh.log` and `minio.log` under the test's temporary directory before changing code. Use superpowers:systematic-debugging. Likely causes, in order: a signed header missing from the request (compare `OSTREE_DEBUG_HTTP=1` output against the headers `_ostree_aws_sign_get` returned), the URL sent differing from the URL signed, a request parked and never resumed.

- [ ] **Step 11: Confirm http(s) and file remotes are unaffected**

Run:
```bash
nix develop -c bash -c 'make check TESTS="tests/test-pull-basicauth.sh tests/test-pull-contenturl.sh tests/test-pull-mirrorlist.sh tests/test-pull-commit-only.sh tests/test-pull-c tests/test-fetcher-s3" 2>&1 | grep -E "^# (TOTAL|PASS|SKIP|FAIL|ERROR):"'
```
Expected: `# FAIL:  0` and `# ERROR: 0`. The shell tests need `ostree-trivial-httpd` (built when libsoup is available); if they report `SKIP`, add `libsoup_3` to `packages` in `flake.nix`, rebuild, and run them again rather than accepting the skip.

- [ ] **Step 12: Commit**

```bash
git add src/libostree/ostree-fetcher-curl.c
git commit -m "lib/fetcher-curl: fetch from s3:// remotes with SigV4-signed requests

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Pull-side concurrency default, option fix, status line

**Files:**
- Modify: `src/libostree/ostree-repo-pull.c` (lines 67, 3608, ~3915, ~4856, 6387)

**Interfaces:**
- Consumes: `_ostree_fetcher_set_max_outstanding_fetcher_requests` (now also sets the curl cap, Task 7).
- Produces: `#define OPT_OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS_S3_DEFAULT 32`. Behaviour: `--max-outstanding-fetcher-requests=N` takes effect for all remotes; `s3://` remotes default to 32.

- [ ] **Step 1: Confirm the failing assertion**

Run:
```bash
nix develop -c bash -c 'make check TESTS=tests/test-pull-s3.sh >/dev/null 2>&1; grep -E "^(ok|not ok)|fatal" tests/test-pull-s3.sh.log | tail -2'
```
Expected: the last line is the assertion-7 failure about connection counts.

- [ ] **Step 2: Fix the option's GVariant type**

The CLI stores this option as `u` (`src/ostree/ot-builtin-pull.c:373`) but both readers ask for `b`, so the lookup always fails and the option is ignored.

Find (around line 3608):

```c
          = g_variant_lookup (options, "max-outstanding-fetcher-requests", "b",
                              &pull_data->max_outstanding_fetcher_requests);
```

Change `"b"` to `"u"`.

Find (around line 6387):

```c
      (void)g_variant_lookup (options, "max-outstanding-fetcher-requests", "b",
                              &max_outstanding_fetcher_requests);
```

Change `"b"` to `"u"`.

- [ ] **Step 3: Add the S3 default**

Find (line 67):

```c
#define OPT_OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS_DEFAULT 8
```

Add after it:

```c
/* S3 only speaks HTTP/1.1, so concurrency comes from parallel connections
 * rather than HTTP/2 multiplexing. */
#define OPT_OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS_S3_DEFAULT 32
```

In `ostree_repo_pull_with_options`, find the comment that follows the block computing `pull_data->content_mirrorlist`:

```c
  /* FIXME: Do we want an analogue of this which supports collection IDs? */
```

Insert immediately before that comment:

```c
  /* The fetcher was created before the URLs were known; raise its
   * concurrency now if content comes from S3 and the caller didn't choose. */
  if (!opt_max_outstanding_fetcher_requests_set)
    {
      OstreeFetcherURI *first_content_uri = pull_data->content_mirrorlist->pdata[0];
      g_autofree char *content_scheme = _ostree_fetcher_uri_get_scheme (first_content_uri);
      if (g_str_equal (content_scheme, "s3"))
        {
          pull_data->max_outstanding_fetcher_requests
              = OPT_OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS_S3_DEFAULT;
          _ostree_fetcher_set_max_outstanding_fetcher_requests (
              pull_data->fetcher, pull_data->max_outstanding_fetcher_requests);
        }
    }

```

Check that `opt_max_outstanding_fetcher_requests_set` is in scope there:

```bash
grep -n 'opt_max_outstanding_fetcher_requests_set' src/libostree/ostree-repo-pull.c
```
Expected: a declaration near line 3536 and uses near 3607 and 3690, all inside `ostree_repo_pull_with_options`, plus the new use. If the compiler disagrees, stop and report rather than moving the declaration.

- [ ] **Step 4: Describe S3 transport in the pull status line**

Find (around line 4856):

```c
      if (g_str_has_prefix (first_scheme, "http"))
        {
          g_string_append (msg, "http: ");
```

and the closing brace of that `if` block, which is followed by `g_string_append (msg, "\n");`. Insert between that closing brace and the `g_string_append (msg, "\n");` line:

```c
      else if (g_str_equal (first_scheme, "s3"))
        g_string_append (msg, "s3: SigV4");
```

- [ ] **Step 5: Build and run the end-to-end test**

Run:
```bash
nix develop -c bash -c 'make -j"$(nproc)" 2>&1 | grep -E "warning|error"; make check TESTS=tests/test-pull-s3.sh 2>&1 | grep -E "^(PASS|FAIL|ERROR|# (TOTAL|PASS|FAIL))"'
```
Expected: seven `PASS:` lines, `# FAIL:  0`.

- [ ] **Step 6: Commit**

```bash
git add src/libostree/ostree-repo-pull.c
git commit -m "lib/pull: default s3:// remotes to 32 concurrent requests

Also fix the GVariant type used to read max-outstanding-fetcher-requests
(\"b\" where the CLI stores \"u\"), which made the option a no-op.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Real-AWS checklist and benchmark script

**Files:**
- Create: `tests/manual/s3-check.sh` (mode 0755)
- Create: `docs/s3-manual-test.md`

**Interfaces:**
- Consumes: a built `ostree` with the `aws` feature.
- Produces: `tests/manual/s3-check.sh pull <s3-url> <ref>` and `tests/manual/s3-check.sh bench <s3-url> <https-url> <ref>`.

These need real buckets and credentials, so the implementer writes and syntax-checks them; a person with AWS access runs them.

- [ ] **Step 1: Write `tests/manual/s3-check.sh`**

```bash
#!/bin/bash
#
# SPDX-License-Identifier: LGPL-2.0+
#
# Manual checks for s3:// remotes against real AWS. See docs/s3-manual-test.md.
#
#   s3-check.sh pull  <s3-url> <ref>
#   s3-check.sh bench <s3-url> <https-url> <ref>
#
# Uses the ostree found in $OSTREE, else in $PATH. Uses whatever AWS
# credentials the environment provides; it never reads or prints them.

set -euo pipefail

ostree=${OSTREE:-ostree}
workdir=$(mktemp -d "${TMPDIR:-/tmp}/s3-check.XXXXXX")
trap 'rm -rf "${workdir}"' EXIT

usage () {
    sed -n '5,10p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

if ! "${ostree}" --version | grep -q -e '- aws$'; then
    echo "error: ${ostree} was built without the aws feature" >&2
    exit 1
fi

# one_pull <label> <url> <ref> [extra pull args...]
# Prints: label, seconds, objects, objects/sec, connections opened.
one_pull () {
    local label=$1 url=$2 ref=$3
    shift 3
    local repo="${workdir}/repo-${label}"
    rm -rf "${repo}"
    "${ostree}" --repo="${repo}" init --mode=bare-user >/dev/null
    "${ostree}" --repo="${repo}" remote add --set=gpg-verify=false origin "${url}"
    local start end
    start=$(date +%s.%N)
    OSTREE_DEBUG_HTTP=1 "${ostree}" --repo="${repo}" pull --disable-static-deltas "$@" origin "${ref}" \
        >/dev/null 2>"${workdir}/debug-${label}.txt"
    end=$(date +%s.%N)
    local objects connections seconds
    objects=$(find "${repo}/objects" -type f | wc -l)
    connections=$(grep -c 'Connected to ' "${workdir}/debug-${label}.txt" || true)
    seconds=$(echo "${end} - ${start}" | bc -l)
    printf '%-14s %8.2fs %7d objects %8.1f obj/s %5d connections\n' \
        "${label}" "${seconds}" "${objects}" "$(echo "${objects} / ${seconds}" | bc -l)" "${connections}"
    # Machine-readable copy for the pass/fail checks in bench.
    echo "${seconds} ${connections}" > "${workdir}/result-${label}.txt"
}

cmd=${1:-}
case "${cmd}" in
    pull)
        test $# -eq 3 || usage
        one_pull s3 "$2" "$3"
        "${ostree}" --repo="${workdir}/repo-s3" fsck >/dev/null
        echo "PASS: pulled $3 and fsck is clean"
        ;;
    bench)
        test $# -eq 4 || usage
        s3_url=$2
        https_url=$3
        ref=$4
        one_pull https-8 "${https_url}" "${ref}" --max-outstanding-fetcher-requests=8
        one_pull s3-8 "${s3_url}" "${ref}" --max-outstanding-fetcher-requests=8
        one_pull s3-default "${s3_url}" "${ref}"
        read -r https_secs _ < "${workdir}/result-https-8.txt"
        read -r s3_8_secs s3_8_conns < "${workdir}/result-s3-8.txt"
        read -r s3_def_secs _ < "${workdir}/result-s3-default.txt"
        fail=0
        # A pull uses up to three fetcher instances, each with its own connections.
        if test "${s3_8_conns}" -gt 24; then
            echo "FAIL: s3 at concurrency 8 opened ${s3_8_conns} connections (limit 24): connections are not being reused"
            fail=1
        fi
        if test "$(echo "${s3_8_secs} > ${https_secs} * 1.05" | bc -l)" = 1; then
            echo "FAIL: s3 at concurrency 8 (${s3_8_secs}s) is more than 5% slower than https (${https_secs}s)"
            fail=1
        fi
        if test "$(echo "${s3_def_secs} >= ${s3_8_secs}" | bc -l)" = 1; then
            echo "FAIL: s3 at the default concurrency (${s3_def_secs}s) is not faster than at 8 (${s3_8_secs}s)"
            fail=1
        fi
        test ${fail} -eq 0 && echo "PASS: benchmark criteria met"
        exit ${fail}
        ;;
    *)
        usage
        ;;
esac
```

Then:
```bash
chmod +x tests/manual/s3-check.sh
```

- [ ] **Step 2: Syntax-check and smoke-test the argument handling**

Run:
```bash
nix develop -c bash -c 'bash -n tests/manual/s3-check.sh && echo syntax-ok; OSTREE=./ostree tests/manual/s3-check.sh; echo "exit=$?"'
```
Expected: `syntax-ok`, then the usage text, then `exit=2`.

If `bc` is not in the dev shell, add `bc` to `packages` in `flake.nix` and include `flake.nix` and `flake.lock` in this task's commit.

- [ ] **Step 3: Write `docs/s3-manual-test.md`**

````markdown
# Manual test checklist for `s3://` remotes

The automated tests cover URL handling, signing, and pulls from a local
S3-compatible server. They cannot cover real AWS credential sources, real
regions, or performance. Run this checklist once in a commercial region and
once in GovCloud before relying on a build.

## Setup

- A build of this fork configured with `--with-curl --with-aws`
  (`ostree --version` lists `- aws`).
- In each partition: a private bucket holding an archive-mode repo under a
  prefix, e.g. `s3://my-bucket/repo`, with a ref to pull.
- The pulling identity needs **both** `s3:GetObject` on
  `arn:…:s3:::my-bucket/repo/*` **and** `s3:ListBucket` on
  `arn:…:s3:::my-bucket`. Without `ListBucket`, S3 answers 403 instead of
  404 for files that do not exist, and ostree treats 403 as a hard error.

Each credential check is:

```
tests/manual/s3-check.sh pull 's3://my-bucket/repo?region=<region>' <ref>
```

and passes when it prints `PASS`.

## Credential sources

| # | Source | How to set it up | Pass |
|---|---|---|---|
| 1 | Static environment keys | `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` for an IAM user | ☐ |
| 2 | EC2 instance role | Run on an instance with an instance profile; no AWS env vars or config | ☐ |
| 3 | EKS IRSA | Run in a pod whose service account is annotated with a role | ☐ |
| 4 | ECS task role | Run in a task with a task role | ☐ |
| 5 | SSO profile | `aws sso login --profile <p>`, then add `&profile=<p>` to the URL | ☐ |
| 6 | Assume-role profile | Profile with `role_arn` and `source_profile`; add `&profile=<p>` | ☐ |
| 7 | `credential_process` profile | Profile with `credential_process = aws configure export-credentials --profile <sso-profile> --format process` | ☐ |

For 2, 3 and 4 also run once **without** `?region=` to confirm the region is
discovered from the environment or instance metadata.

## Error behaviour

| # | Scenario | Expected | Pass |
|---|---|---|---|
| 8 | URL names the wrong region for the bucket | Fails; message says `bucket is in region <actual>` | ☐ |
| 9 | No credentials at all (`AWS_EC2_METADATA_DISABLED=true`, empty config) | Fails with `No AWS credentials available` and the list of sources tried | ☐ |
| 10 | SSO profile whose session has expired (`aws sso logout`) | Fails; message names `aws sso login --profile …` and the `credential_process` alternative | ☐ |
| 11 | Identity lacking `s3:ListBucket` | Fails with `S3 returned HTTP 403 AccessDenied` | ☐ |
| 12 | No region anywhere (unset env, no profile region, off EC2) | Fails; message names `?region=` and `AWS_REGION` | ☐ |

## Long pulls

| # | Scenario | Expected | Pass |
|---|---|---|---|
| 13 | Role session of 15 minutes (`duration_seconds = 900` on an assume-role profile); pull a repo large enough to take longer than that, with `--max-outstanding-fetcher-requests=1` to slow it down | Pull completes; no `ExpiredToken` error | ☐ |

## Performance

Needs a repo of roughly 5,000 objects, and a throwaway **public-read copy**
of it in a test-only bucket in the same region as the client (never make a
real bucket public). Run from a machine in that region:

```
tests/manual/s3-check.sh bench \
  's3://private-bucket/repo?region=<region>' \
  'https://public-copy-bucket.s3.<region>.amazonaws.com/repo' \
  <ref>
```

It prints three timing lines and `PASS: benchmark criteria met`, or one
`FAIL:` line per missed criterion:

- `s3://` at concurrency 8 is within 5% of plain HTTPS;
- connections opened stays within the limit (no reconnect per request);
- `s3://` at the default concurrency (32) is faster than at 8.

| # | Partition | Result | Pass |
|---|---|---|---|
| 14 | Commercial | | ☐ |
| 15 | GovCloud | | ☐ |

If the connection criterion fails, curl is not reusing connections to S3
(see upstream ostree issue #3350). That is a defect to fix in
`ostree-fetcher-curl.c` before this work is considered done; capture the
`OSTREE_DEBUG_HTTP=1` output of one pull and report it.
````

- [ ] **Step 4: Commit**

```bash
git add tests/manual/s3-check.sh docs/s3-manual-test.md
git commit -m "docs: add real-AWS checklist and benchmark script for s3:// remotes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Full regression in both configurations

**Files:** none modified, unless a regression is found.

**Interfaces:**
- Consumes: `/tmp/ostree-s3-baseline-failures.txt` from Task 1.
- Produces: evidence that the suite is no worse than baseline with and without `--with-aws`.

- [ ] **Step 1: Full suite with AWS support**

Run:
```bash
nix develop -c bash -c './configure --with-curl --with-aws >/dev/null && make -j"$(nproc)" >/dev/null && make check -j"$(nproc)" -k > /tmp/ostree-s3-aws-check.log 2>&1; grep -E "^# (TOTAL|PASS|SKIP|FAIL|ERROR):" /tmp/ostree-s3-aws-check.log; grep -E "^(FAIL|ERROR):" /tmp/ostree-s3-aws-check.log | sort > /tmp/ostree-s3-aws-failures.txt; comm -13 /tmp/ostree-s3-baseline-failures.txt /tmp/ostree-s3-aws-failures.txt'
```
Expected: the final `comm` prints nothing (no test fails that did not fail at baseline), and `# TOTAL` is the baseline total plus the new `test-fetcher-s3` cases and `test-pull-s3.sh` assertions.

- [ ] **Step 2: Full suite without AWS support**

Run:
```bash
nix develop -c bash -c './configure --with-curl >/dev/null && make -j"$(nproc)" >/dev/null && ./ostree --version | grep -c -- "- aws"; make check -j"$(nproc)" -k > /tmp/ostree-s3-noaws-check.log 2>&1; grep -E "^(FAIL|ERROR):" /tmp/ostree-s3-noaws-check.log | sort > /tmp/ostree-s3-noaws-failures.txt; comm -13 /tmp/ostree-s3-baseline-failures.txt /tmp/ostree-s3-noaws-failures.txt'
```
Expected: `0` (no `aws` feature), then nothing from `comm`.

- [ ] **Step 3: The no-AWS build refuses `s3://` clearly**

Run (still in the no-AWS configuration):
```bash
rm -rf /tmp/s3-noaws-repo
nix develop -c bash -c './ostree --repo=/tmp/s3-noaws-repo init --mode=bare-user && ./ostree --repo=/tmp/s3-noaws-repo remote add --set=gpg-verify=false origin s3://my-bucket/repo && ./ostree --repo=/tmp/s3-noaws-repo pull origin main 2>&1 | tail -1'
```
Expected: an error line containing `this libostree was built without S3 support`.

- [ ] **Step 4: Restore the AWS configuration and re-run the S3 tests**

Run:
```bash
nix develop -c bash -c './configure --with-curl --with-aws >/dev/null && make -j"$(nproc)" >/dev/null && make check TESTS="tests/test-fetcher-s3 tests/test-pull-s3.sh" 2>&1 | grep -E "^# (TOTAL|PASS|FAIL|ERROR):"'
```
Expected: `# TOTAL: 21`, `# PASS:  21`, `# FAIL:  0`, `# ERROR: 0`.

- [ ] **Step 5: Confirm the tree is clean and summarize**

Run:
```bash
git status --short
git log --oneline fdef7e6..HEAD
```
Expected: no uncommitted changes to tracked files; the spec and plan commits followed by the nine task commits.

If Steps 1-4 required a fix, commit it with a message naming the regression it fixes. Then use superpowers:finishing-a-development-branch.

What remains after this plan, and is not the implementer's to do: running `docs/s3-manual-test.md` against real commercial and GovCloud buckets.
