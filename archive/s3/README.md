# s3:// remotes: development archive

How native `s3://` pull support was designed, built, measured and audited.
Nothing here is built or shipped; the code lives on the `s3-remotes` branch.

## History

This branch joins two histories:

- `s3-fetcher`: the original development, one commit per step, starting from
  the design spec and implementation plan.
- `s3-remotes` up to `b3b793a`: the work squashed into one commit
  (`cea0b70`), then the fixes from each review round.

`s3-remotes` was later rewritten as a series of reviewable commits; this
branch keeps the state before that rewrite.

## Contents

| Path | What |
| --- | --- |
| `design/` | Design spec and implementation plan (2026-10-09) |
| `audit-repro/handoff-audit.md` | The final audit: blockers, follow-ups, and a re-review per fix round |
| `audit-repro/` (rest) | The auditor's repro kit: extra fault modes, in-process pull checker, runner |
| `bench/` | Benchmark harness: cold pulls against S3, a throttling server (`slowserver.py`, `sim.sh`) to replay measured link conditions, A/B against an older build |
| `pr-2-description.md` | Description of PR #2 at `b3b793a`, with the line cost per feature |
| `linecost.py` | Script that attributes added lines to features |

The benchmark scripts were written against an internal bucket. The bucket, the
profile, the refs and the delta path are now required environment
variables (`S3_BENCH_URL`, `AWS_PROFILE`, `S3_BENCH_REF_*`, ...).
