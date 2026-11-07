# Validation checkpoints

The CLI is an experimental Linux x86-64/glibc consumer of immutable published
regex, filesystem and glob sources. It does not change their interfaces.

## Executable checks

- Twelve public CLI methods run in optimized and debug builds with GCC and Clang.
  They cover exact byte output, BFS ordering, empty matches/files, trailing LF,
  CRLF, binary and non-UTF-8 data, terminal escaping, hidden/excluded entries,
  symlinks/FIFOs, pattern/options errors, resource boundaries and stdout failures.
- Python byte-regex fixtures independently check first-match offsets and line
  encoding on shared syntax; they do not establish all-PCRE2 equivalence.
- Full CLI ASan/UBSan configurations pass with both compilers. The separately
  installed PCRE2 binary is not instrumented by these builds.
- Native writer fault tests cover EINTR, short writes, zero-progress/error returns,
  invalid descriptors and invalid span ranges. `/dev/full` and broken pipes are
  exercised through the actual CLI.
- Nine deterministic replacement/growth scenarios cover a selected file vanishing,
  becoming a FIFO/symlink, growing after metadata from empty/nonempty states,
  becoming binary, and growing beyond the file limit. Ordinary and sanitizer
  builds pass; errors after an earlier match publish no result bytes.
- Vendor tampering, injected source and symlinks are rejected even when Python
  optimization is enabled. Native Clang static analysis is clean.

Source `4d1c5c7` introduced the reviewed metadata-sized read and passed the compiler/
sanitizer matrix. The later `edba5a7` checkpoint aligns CLI glob length with its
pinned dependency's 1024-byte ceiling. A fresh standalone clone at that checkpoint
passes check/test/native/race/sanitizer/static-analysis validation with Clang and
a new Goose cache. Later changes clarify documentation and extend exclusion tests.
Hosted exact-commit GCC/Clang CI is required before merging the public candidate.

## Independent review

Two design reviews, two implementation reviews, one read-strategy review and a
final standalone review addressed output failures, byte/accounting bounds, no-follow
races, cleanup, Python verifier optimization, read initialization, growth retries,
pattern boundaries and evidence scope. Findings were fixed and independently
exercised where appropriate. These reviews are not an exhaustive security audit.

## Real inputs and measurements

Pinned Honk, goose-glob and goose-fs Git snapshots plus tiny-file and near-line-cap
fixtures match the independent oracle. `bench/results/initial.json` retains 42
samples. `bench/results/read-strategy.json` retains 84 paired samples, input
manifests, wall time, peak RSS and source/toolchain/backend-version identities.
Historical file hashes have been checked against the corresponding recorded Git
commits. No slower sample is removed and no speed threshold is enforced.

Before/after Callgrind traces retain the 1000-tiny-file profile. Profiling uses
separate 16MiB stack-reservation executables; native timing builds keep defaults.
Instrumented instruction counts, wall time, RSS and allocation volume are distinct.
See bench/README.md for recipes, exact fixture definitions and limitations.

Finite tests do not establish behavior for all filesystems or concurrent schedules,
all regex syntax, every backend version or unsupported platforms. Work limits and
I/O bounds are not wall-clock deadlines. Collection is not a filesystem snapshot.
