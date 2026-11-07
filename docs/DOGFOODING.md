# Everyday search observations

This pass used the released `v0.1.0-alpha.1` on live build-tool, filesystem,
coordination-workspace and web-project trees. Fifteen practical probes covered
function discovery, diagnostics/cleanup references, frontend components/scripts,
documentation, workflow files, legitimate misses and broad generated-output scans.
These are operator observations, not frozen benchmark inputs or performance results.

Captured matches and private working-tree details remain outside this public
repository. No matched lines, private paths, live source snapshots or timing claims
are published here. Reproducible synthetic fixtures live in tests/cli.py.

## Findings and decisions

- Source searches and byte-escaped results worked. Eleven back-to-back candidate/
  tagged-baseline checks preserved exit status and stdout, including no-match and
  intended-limit outcomes. Live inputs can change; this is not snapshot evidence.
- A broad web-project scan reached the 64KiB line cap on generated minified HTML.
  That is an intended failure. Searching the source root or selecting source
  extensions worked. The cap remains unchanged; generated files are not silently
  skipped and no new exclusion is imposed.
- A TypeScript-only glob found nothing in a project whose scripts use JavaScript/
  MJS. That is a valid miss. The tool does not infer extensions.
- Brace globs are literal byte patterns, not extension alternation. They remain
  literal, including support for filenames containing braces. The documentation
  now makes that distinction explicit.
- Relative and trailing-slash roots produced the same vague collection error as
  malformed globs and collection overflow, often followed by `errno=0`. Those
  messages did not identify the corrective action. The fix validates root syntax
  and distinguishes invalid globs and collection limits, while retaining nonzero
  OS errno and omitting meaningless zero errno.

No search policy, resource ceiling, root normalization, binary behavior, stdout
format, exit status or dependency pin changes were warranted. In particular,
there is no new regex API and no reason to advance the regex library release.

## Regressions and validation

Tests reproduce invalid root variants, valid `/` syntax paired with a malformed
pattern (without traversing `/`), malformed glob versus collection overflow,
missing-root nonzero errno, literal-brace filenames, and generated long-line
failure followed by successful source/glob narrowing. Existing positive byte
output, binary/line/call/result budgets, stdout failures and filesystem race tests
remain in place. Fifteen public CLI methods, native writer faults and nine race/
growth scenarios pass in the local GCC/Clang validation configurations.

Independent findings, safety and quality reviews found no remaining blocker.
This diagnostics-only change adds no deadline, sandbox, snapshot, Unicode or
compatibility promise. No new timing benchmark was used to justify it; historical
performance evidence remains tied to its recorded source checkpoints.
