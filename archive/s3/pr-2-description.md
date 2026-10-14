## Summary

Native `s3://bucket/prefix` remotes for `ostree pull`, using the host's AWS credentials. Requests are signed with SigV4 by [aws-c-auth](https://github.com/awslabs/aws-c-auth) and sent through the existing curl fetcher, so `proxy`, `tls-ca-path` and other remote options keep working. Build-time optional: `./configure --with-curl --with-aws`.

This is the squashed, cleaned-up form of the `s3-fetcher` branch, which is kept as the full history (design spec, implementation plan, per-step commits). Usage and design notes are in [`docs/s3-remotes.md`](docs/s3-remotes.md).

## What it does

- **URLs:** `s3://bucket/prefix[?region=][&profile=][&endpoint=]`. Anything else in the URL is rejected.
- **Credentials:** the standard AWS chain: environment, SSO, shared config (static keys, `role_arn`, `credential_process`), web identity (IRSA), then ECS or EC2 instance metadata.
- **Region:** `?region=`, then the environment, the profile, then instance metadata.
- **Errors:** reported with S3's error code. 403 is a hard error, so the identity needs `s3:ListBucket` as well as `s3:GetObject`.
- **Redirects:** never followed for `s3://`, so signed headers can't reach another host.
- **Performance for `s3://`:** 128 requests in flight, no waiting for connection sharing (S3 is HTTP/1.1), 16 delta parts downloading at once with at most 2 applied, and objects over 4 MiB fetched as parallel 4 MiB ranges.

Measured from a workstation to the GovCloud bucket, against the first version of this support: a 9,300-object ref went from about 40 s to 23 s, an 88,000-object ref from 280 s to 100 s, and a 642 MiB static delta from about 130 s to 33 s.

## Changes that also affect non-S3 pulls

Each is small, but reviewers should know about them:

1. **`--max-outstanding-fetcher-requests` now works.** It was read with GVariant type `"b"` while the CLI stores `"u"`, so it was silently ignored. The curl connection cap now follows it: `MAX (8, value)`. A value of 0 means the default.
2. **Delta parts are applied at most 2 at a time.** Finished downloads queue on disk. Previously every finished download was applied immediately, which with more parts in flight drove peak memory from about 90 MiB to 545 MiB.
3. **Metalink entries must have an http(s) URL**, not just a `protocol="http"` label. Otherwise a metalink could point the client at `s3://` and its AWS credentials.
4. **Request headers are rebuilt for every attempt.** This keeps signed headers from ever reaching another mirror, and also stops `If-None-Match` and extra headers being duplicated on mirror retries.

## Safety

- On a normal pull every object is still checksummed before it is stored; nothing here changes that path.
- Ranged fetching keeps `--http-trusted` mirroring (which links files unverified) no weaker than one HTTP response:
  - every ranged response, the first included, must be a 206 whose `Content-Range` names exactly the bytes asked for, with a body of exactly that length;
  - ranges are used only with a strong ETag, which each later range must match through `If-Match`;
  - the assembled size is checked;
  - any failure discards the file.
- No range outlives its object: when an object fails, its other ranges are cancelled before the failure is reported, so they can't keep running (or free the fetcher) after the pull returns.
- `.commitpartial` handling is unchanged. `tests/test-pull-s3-faults.sh` checks, for failed, truncated, ignored, corrupt and mislabelled ranges and for an object changing mid-download, that a failed pull leaves no partial object, keeps the commit marked partial, passes `fsck`, and recovers on the next pull.

## Testing

- `tests/test-fetcher-s3`: 15 cases covering URL parsing and encoding, error and `Content-Range` parsing, signing against independently computed SigV4 signatures, region and credential resolution, and URI validation.
- `tests/test-pull-s3.sh`: 10 cases against a local MinIO (static and temporary credentials, static deltas, ranged objects, connection counts, error cases).
- `tests/test-pull-s3-faults.sh`: 17 cases against a deliberately misbehaving server, including a first range with no, a short or an empty body, a follow-up range that is not the one requested, and several failing pulls in one process (run under Valgrind) to check that no range outlives its pull, and pulls cancelled while their ranges finish.
- The full suite was run with and without `--with-aws`. The failing tests match an untouched upstream build on the same machine (environmental: missing `eu-readelf`, gjs, local test-server flakiness).
- Not automated: real AWS credential sources and the benchmark; see `docs/s3-manual-test.md`.

## Line cost by feature

Added lines (+4,139 / −63 across 25 files), attributed by function or test section. Shared functions were split where their range-handling part begins; expect a few dozen lines of slop between neighbouring rows.

| Feature | Code | Tests | Build | Docs | Total |
|---|---:|---:|---:|---:|---:|
| URL parsing & addressing | 328 | 237 | | | 565 |
| AWS credentials & region | 609 | 152 | | | 761 |
| SigV4 signing | 97 | 74 | | | 171 |
| curl integration (sign, send, errors) | 682 | 275 | | | 957 |
| `s3://` URI validation | 17 | 30 | | | 47 |
| Ranged fetch of large objects | 318 | 637 | | | 955 |
| Concurrency limits | 81 | 21 | | | 102 |
| Delta-part apply queue | 36 | | | | 36 |
| Metalink http(s)-only | 6 | 10 | | | 16 |
| Shared request helpers (refactor) | 60 | | | | 60 |
| Build & dev shell | | | 134 | | 134 |
| Docs & manual checklist | | | | 335 | 335 |
| **Total** | **2,234** | **1,436** | **134** | **335** | **4,139** |

## Review fixes (second commit)

`071a83a` addresses an audit of the first commit:
- **`--max-outstanding-fetcher-requests=0` hung the pull** once the option started working. It now means the default.
- **The first range wasn't checked like the others.** A 206 with no `Content-Range`, or a body shorter than its `Content-Range`, could get a truncated or zero-filled object linked by an `--http-trusted` mirror pull. Normal pulls caught it by checksum. Now rejected, and follow-up ranges must carry a matching `Content-Range` too.
- **Signed headers could in principle survive a fallback to another mirror.** Not reachable today; headers are now rebuilt per attempt.
- **URL edge cases:** IPv6 `?endpoint=` hosts keep their brackets (URL and signed `Host`), and `%2F` in the prefix is rejected. Dot segments needed no change, because GLib's URI parser already removes them, encoded or not; tests pin that.

## Handoff-audit fixes (third commit)

`75a2c19` fixes the two merge blockers from the handoff audit:
- **A failed first range could leave its other ranges running after the pull returned.** In a long-lived process the last of them could free the fetcher from inside its own callback (heap corruption; 12 Valgrind errors in the auditor's repro). Now a failed object's ranges are cancelled at once, a failing range cancels its siblings, and the fetcher holds a reference across curl's callbacks. The auditor's repro now shows 0 Valgrind errors and no leftover temporary files.
- **An empty 206 with no `Content-Range` was accepted as a whole object** under `--http-trusted`. Now rejected at completion too.

It also adds security notes and a quoted example URL to the docs, says in the pull options that 0 means the default, and checks the IPv6 signed `Host` against an independent signature. (`cda3912` untracks a generated file committed by mistake.)

`b3b793a` fixes the one issue the re-review found: **cancelling a pull while its ranges were finishing could fail a range a second time.** `g_task_get_completed()` stays false until the callback has run, which is deferred once the cancellable is cancelled, so a sibling-cancel could re-return a range that had already finished (a GLib critical; an abort under `G_DEBUG=fatal-criticals`). Requests now record that they were returned. Fault case 17 cancels five pulls while nine ranges finish together, with criticals fatal; it aborted before the fix.

Deliberately not in this PR:
- resolving the region from instance metadata asynchronously (today it can block up to 10 s on the first request);
- releasing the old credential provider after a forced refresh (a small leak per refresh);
- moving the ranged-fetch state into its own struct, and the S3 code into its own file.

## Known limitations

- **SSO token refresh:** the library can't refresh an expired SSO token; re-run `aws sso login` or use `credential_process`.
- **Trusted mirroring:** with `--http-trusted`, mirroring stays unverified, as it is for HTTP remotes. Verify-before-link was considered and deliberately left out.
- **Mirrorlists** ignore `s3://` entries.
- **`min-free-space`** is enforced at write time, not while objects download.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

