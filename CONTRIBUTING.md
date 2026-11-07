# Contributing

Keep the CLI small and explicit. Preserve byte semantics, finite budgets,
cleanup-before-publication, no-follow reads and checked output errors. Do not add
implicit shells, global signal policy, callbacks, hidden retries or lossy success.
Discuss interface/limit changes first. Do not imply snapshots or deadlines.

Use feat/..., fix/..., test/... or docs/... branches and focused Conventional
Commits. Commit working changes regularly. Keep binaries, caches, credentials,
private coordination notes and unrelated work out of commits.

Run `make format check test native test-races` with GCC and Clang, then both
`sanitize` and `sanitize-races` configurations. Native changes need static analysis
(`clang --analyze native/output.c`). Behavior, ownership and meaningful performance
changes require independent review and regressions. Keep hosted CI passing.
Use the full upstream compiler revision in GOOSE_REVISION.

Never copy dirty sibling dependencies or modify pinned vendored files. A deliberate
update needs the exact public commit, per-file hashes, licenses/notices, review and
full validation. Compiler wrapper identities include their own sources, instrumentation,
fault sources where applicable, and the real compiler executable. Do not mutate
an ignored wrapper in place. Refresh native fingerprints with `make format`.

Profile before optimizing. Keep raw inputs/manifests, exact sources/toolchain/
backend identities, slower samples and unsuccessful qualified attempts. Distinguish
instrumented instruction counts from native wall time, and RSS from allocations.
Do not enforce CI speed thresholds or claim universal performance. Published tags
must never move; no compatibility guarantee is made before 1.0.
