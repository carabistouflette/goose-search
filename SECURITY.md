# Security

Report suspected vulnerabilities through
[GitHub private vulnerability reporting](https://github.com/carabistouflette/goose-search/security/advisories/new).
Private reporting is enabled. Include the affected full commit, Goose/PCRE2
versions, platform, arguments and a minimal reproducer. Keep exploit details out
of public issues until a fix is available. The experimental interface has no
long-term support or pre-1.0 compatibility promise.

Treat patterns, file contents, paths and terminal output as bytes. Bounds and
escaping reduce accidental resource exhaustion and terminal injection; they do
not provide deadlines, snapshots, confinement, a cryptographic guarantee or a
complete security audit. This tool reads and discloses matching lines. Never use
it as an access-control or secret-redaction boundary.

The underlying Root operations reject symlinks but hard links, mounts, directory
moves, concurrent writes and device callbacks remain possible. Selected files can
change between collection, metadata and read operations; the bounded retry is not
snapshot validation. A malicious filesystem or regex can still take substantial
time. Externally supervise processes when a hard deadline is needed.

Output is retained until search and cleanup succeed. Publication itself can fail
partway, and inherited SIGPIPE behavior remains. Catastrophic runtime/OS failures
can terminate the process outside the normal exit2 error protocol. Native ABI
callers must supply valid accessible memory; range checks do not prove provenance.

Use security-maintained PCRE2 packages. The vendored regex/FS/glob code and compiler
are pinned; updates require deliberate review. ASan/UBSan instrument the wrappers,
not the separately installed PCRE2 binary. Finite tests and profiling are evidence,
not exhaustive assurance. See README.md for the precise limits and error contract.
