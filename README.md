# goose-search

A small bounded file-search CLI for [Goose](https://github.com/aardappel/goose).
Compile a byte regex once, select files by path glob, and print the first match
on each matching line. Experimental, Linux x86-64/glibc, Apache-2.0.

## Build and search

Requires the pinned Goose compiler, CMake 3.27+, C/C++20 tools for building Goose,
Make, Git, Python 3.11+, pkg-config, PCRE2 development files >=10.42, and Linux
headers with openat2. Clang is needed for the second validation configuration.

```sh
git clone https://github.com/carabistouflette/goose-search.git
cd goose-search
make setup
make build
.build/goose-search --glob '**/*.goose' -- 'fn [A-Za-z_][A-Za-z0-9_]*' "$PWD"
```

An existing compiler matching `GOOSE_REVISION` can be supplied with
`GOOSE=/absolute/path/to/goose`. All three libraries are vendored from immutable
public commits; builds do not read dirty sibling checkouts or download packages.
`make setup` fetches and builds the pinned compiler. PCRE2 remains a system
backend: after changing its version/linkage, use a fresh `GOOSE_HOME` to avoid
reusing Goose's native package cache from another backend.

## Command and output

```text
goose-search [--glob PATTERN] [--binary skip|search] -- REGEX ABSOLUTE_ROOT
```

The `--` separator is required. `--glob` defaults to `**` and uses goose-glob's
byte path syntax. Each option may occur once. Roots must be strict absolute
paths: no symlink components, `.`, `..`, repeated/trailing separators (except `/`)
or NUL; path size <=4095 bytes. The CLI does not normalize paths.

A result is `escaped-relative-path:line:byte-column:escaped-line` followed by LF.
Line and column are 1-based; only the first match is reported for each line.
Columns count bytes. A zero-length match at line end reports `line.len+1`.
Paths escape colon, backslash, control and non-ASCII bytes as lowercase `\xNN`.
Content escapes backslash, control and non-ASCII bytes. Terminal control bytes
are never emitted from filenames/content. Output discloses matching file lines;
there is no masking or redaction.

Exit status is 0 for matches, 1 for no matches, and 2 for errors. Search/setup/
cleanup errors produce stderr and no result bytes. Results are buffered until
the complete search and resource cleanup succeed. A final stdout failure can
leave a prefix and reports 2 if the process survives the write. Inherited SIGPIPE
behavior can instead terminate a broken pipeline with its shell signal status.
There is no streaming, timeout, color, replacement, stdin search or gitignore
handling in this version.

## Selection and byte semantics

Hidden paths are skipped. Entries named `vendor`, `target` or `node_modules`
are excluded at any depth, and excluded directories are pruned. Choose such a
directory as the root to search its contents explicitly. Results follow breadth-first directory order, with unsigned-byte
sorting inside each directory; they are not globally sorted.

NUL anywhere in a bounded file classifies it as binary. Default `--binary skip`
skips that whole file before line validation or matching, including matches that
precede a late NUL. Skipped bytes still count toward total input. Oversized files
fail before binary classification. `--binary search` treats NUL as ordinary data;
non-UTF-8 bytes are also ordinary data and are escaped in output.

Lines are LF-delimited subjects with LF removed and CR retained. Anchors and
lookarounds apply separately to each line. Empty files contain zero lines, `\n`
contains one empty line, and a final LF does not add a phantom line. Empty regex
matches are valid without an iteration policy because each line yields at most
one result. UTF/UCP activation is disabled by goose-regex; PCRE2 byte syntax and
resource semantics apply. There is no Unicode character-column or multiline-file
matching promise.

## Bounds

| Resource | Fixed ceiling |
| --- | --- |
| Regex / path-glob pattern bytes | 16,384 / 1,024 |
| Selected files / visited entries / depth | 1,000 / 10,000 / 32 |
| Shared glob work budget / pattern components | 1,048,576 / 128 |
| Retained result-path bytes / queued directory-path bytes | 1 MiB / 16 MiB |
| Entries / framed name bytes in one directory | 4,096 / 1 MiB |
| One file / one line excluding LF | 1 MiB / 64 KiB |
| Total accepted file-content bytes | 64 MiB |
| Regex calls / matching lines | 1,000,000 / 10,000 |
| Encoded result bytes | 8 MiB |
| PCRE2 allocated bytes / work / depth per handle | 8 MiB / 100,000 / 1,000 |

Limits fail explicitly; no partial result is reported as complete. Visited
entries include entries later skipped by filtering. Underlying glob work and
filesystem pathname limits also apply. Metadata sizes the first read; a file
that grows can cause one bounded retry up to the per-file/remaining total ceiling.
Read overflow probes and failed attempts are not counted as accepted content
bytes. These bounds do not describe process RSS or all physical I/O.

PCRE2 work counters reset for candidate starting positions; they are not total
instruction or wall-clock limits. Compilation and OS I/O have no hard deadlines.
Use external process supervision if a hard deadline is required.

Collection and subsequent Root-relative reads are separate observations. Reads
reject symlink ancestors/final links and nonregular files; files disappearing or
changing type cause errors. Root identity and file contents are not a snapshot.
Hard links, mounts and concurrent renames remain possible, so this is not a
sandbox or an authorization check. Device-open callbacks may occur before the
filesystem library's regular-file check.

## Validation and evidence

```sh
make check test native test-races CC=gcc
make sanitize sanitize-races CC=gcc
make check test native test-races CC=clang
make sanitize sanitize-races CC=clang
```

Run Make invocations serially within one checkout. Tests cover exact bytes,
empty/CRLF/long/binary inputs, resource limits, a Python byte-regex oracle,
terminal-safe output, stdout failure, EINTR/short writes, selected-file races and
growth. Sanitizers cover the CLI/native wrappers, not the system PCRE2 binary.
[Measurements](bench/README.md) retain pinned real-tree manifests, raw wall-time/
peak-RSS samples and profiles. No universal speed or security-audit claim follows.
The regex library interface did not need changes for this consumer.

[Design](DESIGN.md) · [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md)
· [Dependency notices](THIRD_PARTY_NOTICES.md)
