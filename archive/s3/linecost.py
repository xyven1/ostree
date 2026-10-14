"""Attribute every added line of the staged diff to a feature.

C files are split by top-level function (or struct/define) using the final
file; shell tests by their numbered "# N:" sections; everything else by file.
Counts are added lines including comments and blanks; removed lines are
reported separately per file.
"""
import collections
import re
import subprocess
import sys

BASE = "fdef7e6"


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def added_lines(path):
    """Line numbers (1-based, in the new file) of added lines."""
    out = run("git", "diff", "--cached", "-U0", BASE, "--", path)
    nums = []
    for m in re.finditer(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", out, re.M):
        start, count = int(m.group(1)), int(m.group(2) or 1)
        nums.extend(range(start, start + count))
    return nums


def removed_count(path):
    out = run("git", "diff", "--cached", "--numstat", BASE, "--", path)
    return int(out.split()[1]) if out.strip() else 0


def c_spans(path):
    """Map line number -> enclosing top-level definition name ('' if none)."""
    lines = run("git", "show", f":{path}").split("\n")
    owner = [""] * (len(lines) + 2)
    current = ""
    for i, line in enumerate(lines, 1):
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*) \(", line)
        s = re.match(r"^(?:struct|typedef struct) ?([A-Za-z0-9_]*)", line)
        d = re.match(r"^#define ([A-Z0-9_]+)", line)
        if m:
            current = m.group(1)
        elif s and line.rstrip().endswith(("{", "")) and not line.rstrip().endswith(";"):
            current = "struct " + (s.group(1) or "?")
        elif d:
            owner[i] = "#define " + d.group(1)
            continue
        owner[i] = current
        if line == "}" or line.startswith("} "):
            current = ""
    return owner, lines


# Within these shared functions, lines from the first match of the start marker up
# to (not including) the stop marker belong to the given feature.
SPANS = [
    ("s3_write", r"s3_range_parent != NULL", r"^  return FALSE;", "ranged fetch of large objects"),
    ("s3_header", r"s3_range_part > 0", r"^}", "ranged fetch of large objects"),
    ("s3_begin_attempt", r"s3_range_part = 0;", r"^  return TRUE;", "ranged fetch of large objects"),
    ("check_multi_info", r"s3_range_parent != NULL\)", r"else if \(req->is_membuf\)", "ranged fetch of large objects"),
    ("check_multi_info", r"s3_range_pending \|\|", r"^              else$", "ranged fetch of large objects"),
]


def span_feature(name, text, n):
    for func, start, stop, feat in SPANS:
        if name != func:
            continue
        # walk back to the function start, then forward to find the window
        i = n - 1
        while i > 0 and not re.match(r"^%s \(" % func, text[i]):
            i -= 1
        inside = False
        for j in range(i, len(text)):
            if not inside and re.search(start, text[j]):
                inside = True
            elif inside and re.search(stop, text[j]):
                inside = False
            if j == n - 1:
                if inside:
                    return feat
                break
    return None


def classify_c(path, rules, default):
    owner, text = c_spans(path)
    counts = collections.Counter()
    for n in added_lines(path):
        name = owner[n] if n < len(owner) else ""
        line = text[n - 1] if n - 1 < len(text) else ""
        feature = span_feature(name, text, n)
        if feature:
            counts[feature] += 1
            continue
        for pattern, feat in rules:
            if re.search(pattern, name) or (pattern.startswith("line:") and re.search(pattern[5:], line)):
                feature = feat
                break
        counts[feature or default] += 1
    return counts


def classify_sections(path, sections, default):
    """Shell tests: '# N:' markers start a section."""
    text = run("git", "show", f":{path}").split("\n")
    current = default
    owner = []
    for line in text:
        m = re.match(r"^# (\d+):", line)
        if m:
            current = sections.get(int(m.group(1)), default)
        owner.append(current)
    counts = collections.Counter()
    for n in added_lines(path):
        counts[owner[n - 1]] += 1
    return counts


F_URL = "URL parsing & addressing"
F_AWS = "AWS credentials & region"
F_SIGN = "SigV4 signing"
F_CURL = "curl integration (sign, send, errors)"
F_RANGE = "ranged fetch of large objects"
F_CONC = "concurrency limits"
F_DELTA = "delta-part apply queue"
F_META = "metalink http(s)-only"
F_VALID = "s3:// URI validation"
F_BUILD = "build & dev shell"
F_DOCS = "docs & manual checklist"
F_REFAC = "shared request helpers (refactor)"

totals = collections.defaultdict(lambda: collections.Counter())  # feature -> {code,test,docs}


def add(counts, kind):
    for feat, n in counts.items():
        totals[feat][kind] += n


add(classify_c("src/libostree/ostree-fetcher-s3.c", [
    (r"parse_u64|content_range", F_RANGE),
    (r"credentials_usable", F_AWS),
    (r"error_body|S3ErrorParse|s3_error_", F_CURL),
], F_URL), "code")
add(classify_c("src/libostree/ostree-fetcher-s3.h", [
    (r"line:content_range|Content-Range|START-END", F_RANGE),
    (r"line:credentials_usable|CREDS_|expiry|fetched_at", F_AWS),
    (r"line:error_body|error document|out_code", F_CURL),
], F_URL), "code")
add(classify_c("src/libostree/ostree-aws.c", [
    (r"sign|SignState", F_SIGN),
], F_AWS), "code")
add(classify_c("src/libostree/ostree-aws.h", [
    (r"line:sign", F_SIGN),
], F_AWS), "code")
add(classify_c("src/libostree/ostree-fetcher-curl.c", [
    (r"s3_range|line:s3_range|line:S3_RANGE|line:Range|line:If-Match|content_range", F_RANGE),
    (r"^request_(new|task_new|finished|fail_later)$|FailLater|fail_later", F_REFAC),
    (r"set_max_outstanding|line:PIPEWAIT", F_CONC),
], F_CURL), "code")
add(classify_c("src/libostree/ostree-repo-pull.c", [
    (r"deltapart_appl|line:pending_deltapart|line:pending_fd|line:MAX_APPLYING|line:fetch_static_delta_data_free|line:deltas_full|line:Parts that are downloaded|line:so that downloads|line:n_outstanding_deltapart_fetches", F_DELTA),
    (r"line:max_outstanding|line:S3|line:s3|line:content_scheme|line:first_content_uri|line:fetcher was created|line:now if content|line:Delta parts can never|line:throughput comes|line:multiplexing", F_CONC),
    (r"line:s3:", F_CONC),
], F_CONC), "code")
add(classify_c("src/libostree/ostree-repo-pull-private.h", [], F_DELTA), "code")
add(classify_c("src/libostree/ostree-fetcher-uri.c", [], F_VALID), "code")
add(classify_c("src/libostree/ostree-metalink.c", [], F_META), "code")

add(classify_c("tests/test-fetcher-s3.c", [
    (r"content_range", F_RANGE),
    (r"test_sign|find_header", F_SIGN),
    (r"resolve_region|credentials_chain|clear_aws_env|on_creds|get_creds_sync|credentials_usable", F_AWS),
    (r"error_body", F_CURL),
    (r"uri_validate", F_VALID),
], F_URL), "test")
add(classify_sections("tests/test-pull-s3.sh", {7: F_CONC, 9: F_CURL, 10: F_RANGE}, F_CURL), "test")
for p in ("tests/test-pull-s3-faults.sh", "tests/s3-fault-server.py", "tests/s3-pull-repeat.c"):
    add({F_RANGE: len(added_lines(p))}, "test")
add({F_CURL: len(added_lines("tests/libtest-s3.sh"))}, "test")
add({F_META: len(added_lines("tests/test-pull-metalink.sh"))}, "test")

for p in ("configure.ac", "Makefile-libostree.am", "Makefile-tests.am", "flake.nix", "flake.lock", "tests/.gitignore"):
    add({F_BUILD: len(added_lines(p))}, "build")
for p in ("docs/s3-remotes.md", "docs/s3-manual-test.md", "tests/manual/s3-check.sh"):
    add({F_DOCS: len(added_lines(p))}, "docs")

order = [F_URL, F_AWS, F_SIGN, F_CURL, F_VALID, F_RANGE, F_CONC, F_DELTA, F_META, F_REFAC, F_BUILD, F_DOCS]
print(f"{'feature':40} {'code':>6} {'tests':>6} {'build':>6} {'docs':>6} {'total':>6}")
grand = collections.Counter()
for f in order:
    c = totals.get(f, collections.Counter())
    tot = sum(c.values())
    grand.update(c)
    print(f"{f:40} {c['code']:6} {c['test']:6} {c['build']:6} {c['docs']:6} {tot:6}")
print(f"{'TOTAL':40} {grand['code']:6} {grand['test']:6} {grand['build']:6} {grand['docs']:6} {sum(grand.values()):6}")
unknown = set(totals) - set(order)
if unknown:
    print("UNCLASSIFIED:", unknown, file=sys.stderr)
print("\nper file (added / removed):")
for line in run("git", "diff", "--cached", "--numstat", BASE).strip().split("\n"):
    a, r, p = line.split("\t")
    print(f"  {a:>5} / {r:<4} {p}")
