# s3:// remotes PR #2 — handoff audit

Oct 14, 2026 · @Blake

[xyven1/ostree#2](https://github.com/xyven1/ostree/pull/2) at `b3b793a7` has no open blockers or new findings, and its S3 suites and repros pass. What is left are the deferred follow-ups, to do before upstreaming. Earlier sections record the audits of `cda39128` and `071a83a`.

## Re-review at `b3b793a7`

`b3b793a7` fixes the cancellation CRITICAL. Requests now record that they were returned, in `request_finished()`, and the range code checks that flag instead of `g_task_get_completed()`. Every place a task is returned calls `request_finished()` straight after, so the flag is accurate.

| Check | Result |
| --- | --- |
| `test-fetcher-s3`, `test-pull-s3.sh` (MinIO), `test-pull-s3-faults.sh` | 42 of 42 pass (15 + 10 + 17) |
| My cancelled-pull repro, 10 runs | 0 criticals (2 of 3 runs hit them on `cda39128`) |
| The same, under Valgrind, 2 runs | 0 memory errors, 0 criticals |
| Repro kit: `firstempty`, `firstdrop` under Valgrind | Still rejected; 0 errors |
| Full regression suite, `make check -j16` | 769 pass, 21 skip; 42 test files failed |
| Those 42 rerun serially | 28 pass. All 14 left are environmental: the 8 known on this machine, plus 6 whose local test server refused connections (`[7] Could not connect` on `summary.sig`) |

Not run: real AWS (the `docs/s3-manual-test.md` checklist) and a baseline build for those 6 environmental failures.

## Re-review at `cda39128`

Commits `75a2c197` and `cda39128` fix both blockers, add the fault cases and the in-process test, and close follow-ups 1–4, 8 and 10.

| Check | Result |
| --- | --- |
| `test-pull-s3-faults.sh` (16 cases, case 16 under Valgrind) | 16 of 16 pass |
| `tests/test-fetcher-s3`, now with the IPv6 signed-`Host` vector | 15 of 15 pass |
| Repro kit, `firstempty` | Rejected: "ranged response without a usable Content-Range"; nothing linked |
| Repro kit, `firstdrop` under Valgrind | 0 errors (was 12) |
| Three pulls cancelled at 0.5 s while 9 ranges finish together | 2 of 3 log `g_task_return_error: assertion '!task->ever_returned' failed` (1 and 5 times) |
| The same three cancelled pulls under Valgrind | 0 memory errors, no criticals (timing differs) |

**Remaining issue: double return after cancellation.** `s3_range_cancel_children` and `fail_later_idle_cb` use `g_task_get_completed()` to mean "already returned". GLib sets that flag only after the callback runs. Once the task's cancellable is cancelled, the callback is deferred to an idle, so a range that has already returned still reads as not completed (checked on GLib 2.88.3). Children share the pull's cancellable. After a cancel, the first child callback sees `CANCELLED` and cancels its siblings, re-returning any that finished in the same batch.

GLib's guard prevents a second callback, so there is no memory error and the counts stay right. The cost is a CRITICAL in the log, or an abort where criticals are fatal.

Fix: record that a request has been returned in `FetcherRequest` (set wherever a task is returned) and test that flag instead of `g_task_get_completed()` in both places. A shell-level test would need a cancellable, so extend `s3-pull-repeat` with a cancel-after-delay mode and fail on any CRITICAL.

Not rerun: the MinIO suite, the full regression suite and real AWS. Not checked: whether test 16 fails on `071a83a`. My separate repro did fail there, and its scenario matches.

## Status at handoff

Two items block merge, one of them new in this round. Of the round 1 findings, five are fixed in `071a83a`, one was a false alarm, and four are deferred.

| Finding | Severity | Status | Where |
| --- | --- | --- | --- |
| Use-after-free / leak when a pull ends with a failed parent range | Blocker | Fixed in 75a2c197 | `ostree-fetcher-curl.c:411`, `request_finished` |
| Empty `206` with no `Content-Range` accepted as a whole object | Blocker for `--http-trusted` | Fixed in 75a2c197 | `ostree-fetcher-curl.c:919`, `s3_write` |
| `--max-outstanding-fetcher-requests=0` hangs the pull | High | Fixed in `071a83a` | `ostree-repo-pull.c:3667` |
| First range not checked (missing `Content-Range`, short body) | High | Fixed in `071a83a` | `s3_write`, `check_multi_info` |
| Child range not checked against the offset asked for | Medium | Fixed in `071a83a` | `s3_header` |
| Signed headers carried over to a non-S3 mirror | Low (unreachable) | Fixed in `071a83a` | `initiate_next_curl_request` |
| IPv6 `?endpoint=` loses its brackets | Low | Fixed in `071a83a` | `normalize_endpoint`, `_ostree_aws_sign_get_at` |
| `%2F` in the key prefix becomes a separator | Low | Fixed in `071a83a` | `_ostree_s3_location_parse` |
| Dot segments in the prefix break signing | — | Not a bug (GLib's parser removes them) | tests pin it |
| Sibling ranges keep running after one fails | Medium | Deferred | `s3_range_child_done` |
| Region lookup from instance metadata blocks up to 10 s | Medium | Deferred | `ostree-aws.c:509` |
| Credential provider chain leaked per forced refresh | Low | Deferred | `ostree-aws.c:328` |
| Ranged-fetch state as 9 loose fields; S3 code inside the curl fetcher | Clarity | Deferred | `FetcherRequest` |

## Fix before merge

### 1. A failed parent range leaves orphans that later corrupt the heap

If the request for the first 4 MiB of a large object fails while its follow-up ranges are still downloading, those ranges outlive the pull. In a long-lived process they later free the fetcher from inside its own callback.

**How it happens**

1. The parent fails mid-transfer (a dropped connection, the low-speed timeout, or one of `071a83a`'s new first-range checks) and returns its error at once.
2. The pull never counted the child ranges, so it can finish and return while they still run. Each child holds the parent task, and the parent task holds the fetcher.
3. The children's sockets stay on the caller's thread-default main context, which the pull runs on (`ostree-repo-pull.c:3734`).
4. The caller iterates that context again (a second pull, or a daemon's main loop), and the last child finishes.
5. `request_finished` removes the child from `outstanding_requests` (`ostree-fetcher-curl.c:411`). That drops the last reference on the parent, then the last on the fetcher.
6. `_ostree_fetcher_finalize` frees the hash table that `g_hash_table_remove` is still walking, and `check_multi_info` carries on using the freed fetcher.

**Evidence**

- Three failing pulls in one process (fault mode `firstdrop`, `n-network-retries=0`): in one of two plain runs (heap corruption doesn't always crash), glibc aborted with `malloc_consolidate(): unaligned fastbin chunk detected`.
- Under Valgrind: 12 errors in 11 contexts on every run, including an invalid write. The block was freed in `_ostree_fetcher_finalize`, reached through `s3_request_clear` → `request_unref`, and read again in `g_hash_table_remove` called from `request_finished`.
- When the context is never iterated again, each failed pull instead leaks two sockets and the parent's unlinked temp file (up to the object's size on disk).

**Who is exposed:** the `ostree` CLI exits, so it is unaffected. Long-lived library users that reuse a thread's main context are exposed: rpm-ostreed, the flatpak system helper, bootc, GI callers. Realistic triggers are the last retry failing on a flaky link, another object failing and ending the pull, or cancellation.

**Fix**

1. Never complete a parent before its children. On a parent error, store it in `s3_range_error`, set `s3_range_self_done`, and complete the parent when the child count reaches 0, the way the success path already does. The pull's counters then cover every transfer.
2. Cancel sibling ranges on the first failure: remove each child's easy handle and fail it. This was deferred as an optimisation; with step 1 it is needed so a failed object doesn't wait out every range.
3. Defensive: hold a reference on the fetcher in `event_cb` and `timer_cb` across `check_multi_info`, so a fetcher can never finalize inside its own callback.
4. Test: the shell fault tests can't see this because the CLI exits. Add an in-process C test (the repro checker below is a starting point) and run it under Valgrind or ASan.

### 2. An empty `206` still passes the first-range checks

Under `--http-trusted`, a `206` with `Content-Length: 0` and no `Content-Range` still mirrors a 0-byte object and exits 0. fsck then reports "File header size is zero".

The "206 needs a usable `Content-Range`" check lives in `s3_write` (`ostree-fetcher-curl.c:919`), which only runs when body bytes arrive. Repeat it in `check_multi_info`'s success path: if a range was requested, the response is 206 and `s3_range_total == 0`, fail the request. Add fault mode `firstempty` to `tests/s3-fault-server.py` with a case in `test-pull-s3-faults.sh`.

Like the other first-range faults, this needs a server that breaks HTTP. A normal pull rejects the object by checksum, so the exposure is limited to trusted mirroring.

## Open follow-ups

None of these makes a pull produce wrong results. Sibling cancellation moves into the blocker fix; the rest are ranked by when they should land.

| # | Item | Why it matters | When |
| --- | --- | --- | --- |
| 1 | Cancel sibling ranges on first failure | Needed by blocker 1's fix; otherwise a failed object waits out every range | With blocker 1 |
| 2 | Docs: Integrity section | It says every ranged response is checked for `Content-Range` and length; false until blocker 2 is fixed | With blocker 2 |
| 3 | Docs: security notes | `?endpoint=http://` sends `Authorization` and `X-Amz-Security-Token` in clear text. `OSTREE_DEBUG_HTTP` prints both. Anyone who can configure a remote can direct signed requests (host-bound, so not replayable against AWS, but the access key ID and session token are exposed) | Before merge |
| 4 | Docs: quote the example URL | `ostree remote add ... s3://my-bucket/repo?region=...` fails in fish and zsh, which treat `?` as a glob | Before merge |
| 5 | Release the old provider chain on forced refresh | Leaked STS, ECS and instance-metadata providers own HTTP connection managers, so the leak may include sockets, not just a few KB. Confirm against aws-c-auth's lifetime rules | Before upstreaming |
| 6 | Resolve the region from instance metadata asynchronously | Blocks the caller's main loop up to 10 s; upstream will object | Before upstreaming |
| 7 | Ranged-fetch state into its own struct; S3 code into its own file | Both blockers live in the 9 loose `s3_range_*` fields; a state enum makes the completion paths checkable | Own PR, before upstreaming |
| 8 | Test gaps | No test of the signed `Host` header for IPv6 endpoints; no fault case where the parent fails after children start | With blockers |
| 9 | `404` errors drop the S3 code | `NoSuchBucket` and `NoSuchKey` both read "Server returned HTTP 404" | Any time |
| 10 | Document that `0` means the default | Said in `docs/s3-remotes.md`, not in the pull-options doc comment (`ostree-repo-pull.c:3532`) | Any time |

## What was verified

All runs used the PR head `071a83a`, built at `.claude/worktrees/s3-fetcher`. Faults came from a local server with S3-style ranges and a 10 MiB object (three 4 MiB ranges).

| Check | Result |
| --- | --- |
| `tests/test-fetcher-s3` unit suite | 15 of 15 pass |
| `--http-trusted` mirror, `206` without `Content-Range` | Rejected: "Ranged response without a usable Content-Range" |
| `--http-trusted` mirror, first body 1000 bytes of 4 MiB | Rejected: "ranged response has 1000 bytes, expected 4194304" |
| `--http-trusted` mirror, empty `206` | **Accepted**: pull exits 0, 0-byte object, fsck fails |
| Two in-process pulls, well-behaved server, under Valgrind | 0 errors; no sockets or temp files left |
| In-process pulls, parent drops mid-range | **Heap corruption**: 12 Valgrind errors; one glibc abort in two plain runs |
| `--max-outstanding-fetcher-requests=0` | Hung on `cea0b705`; `071a83a` treats 0 as the default (code read, not rerun) |

Not run in this round: the MinIO end-to-end suite, the full regression suite, and real AWS. The previous session reported 10 MinIO and 14 fault cases passing, and no new full-suite failures.

## Repro kit

Everything needed to reproduce both blockers is in `s3-audit-repro/` on the dev machine, outside the repo.

- `s3-fault-server-extra.py`: `tests/s3-fault-server.py` plus four first-range modes: `firstnorange`, `firstshortbody`, `firstempty` and `firstdrop` (the first range drops after 1 MiB while child ranges answer after 5 s).
- `inproc-pull.c`: runs several failing pulls in one process through libostree and prints open sockets and unlinked temp files after each.
- `run.sh <build dir> <mode> [valgrind]`: builds a repo with a 10 MiB object, starts the server and runs the scenario.

```
s3-audit-repro/run.sh <worktree> firstempty          # blocker 2: pull exits 0, fsck fails
s3-audit-repro/run.sh <worktree> firstdrop valgrind  # blocker 1: Invalid read/write
```

Use Valgrind mode for blocker 1: a plain run only sometimes aborts. After the fix, both commands should fail the pull cleanly, and Valgrind should report 0 errors. `firstnorange` and `firstshortbody` are the regression checks for what `071a83a` fixed.
