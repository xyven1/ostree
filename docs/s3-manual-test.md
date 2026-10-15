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
- `s3://` at the default concurrency (128) is faster than at 8.

| # | Partition | Result | Pass |
|---|---|---|---|
| 14 | Commercial | | ☐ |
| 15 | GovCloud | | ☐ |

If the connection criterion fails, curl is not reusing connections to S3
(see upstream ostree issue #3350). That is a defect to fix in
`ostree-fetcher-curl.c` before this work is considered done; capture the
`OSTREE_DEBUG_HTTP=1` output of one pull and report it.
