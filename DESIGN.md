# Bounded file-search consumer

This CLI exercises goose-glob, goose-fs and goose-regex without adding a regex
interface. Compile the regex once; collect relative paths; reopen each selected
file with no-follow regular-file reads; return the first match on each line.

Invocation: `goose-search [--glob PATTERN] [--binary skip|search] -- REGEX ABSROOT`.
No stdin, shell, configuration file, color, iterator or replacement. Hidden paths
are skipped. Exclude entries named vendor, target and node_modules at any depth
and prune excluded directories; choose one of
those directories as the root to search it explicitly. Collection is sorted BFS,
not global path sort. No gitignore handling or snapshot/sandbox guarantee.
Collection and later reads are separate observations; root identity is not
promised stable between them. Hard links, mounts and concurrent changes remain
possible. Replaced nonregular/symlink selected files cause errors. The underlying
filesystem may invoke device callbacks before its regular-file check.

Fixed limits: 16384 regex bytes, 1024 glob bytes, 1000 selected files, 10000 visited entries, depth32, 1MiB retained
path bytes, 1MiB/file, 64KiB/line excluding LF, 64MiB total accepted file bytes,
1million regex calls, 10000 matching lines, 8MiB encoded output. Filesystem reads
can probe one extra byte to detect overflow. Metadata sizes the initial read;
one bounded retry up to the remaining ceiling handles concurrent growth. Failed
attempts/probes do not count as accepted content bytes. PCRE2 work limit100000 per candidate
start and depth1000 are not deadlines. OS I/O has no deadline either.

NUL anywhere in a bounded file means binary. Default skip occurs before line
checks; skipped binary bytes still consume the aggregate input budget. Oversized
files fail before binary classification. Search mode treats all bytes as content.
Lines split on LF, retaining CR. Empty files contain zero lines, LF-only files
contain an empty line, and a final LF adds no phantom line. Empty regex matches
are valid; emitting one first match per line avoids iteration/retry semantics.

Output: escaped-relative-path:1-based-line:1-based-byte-column:escaped-line plus
LF. Escape path colon, backslash, control and non-ASCII bytes as lowercase \xNN;
content escapes backslash/control/non-ASCII. Match capture zero defines the byte
column; captures otherwise remain internal. All results are buffered; successful
search and checked resource cleanup precede publication. Scan/setup/close errors
publish no stdout. A final write failure may expose a prefix. Checked POSIX writes
retry EINTR/short writes; default SIGPIPE behavior is retained. Exit0 matches,
1 no matches,2 errors (signal termination can report shell signal status).

Tests must cover binary/long/empty/malformed inputs, terminal-safe output, exact
boundaries, symlink/FIFO roots/entries, output failures, differential common regex
fixtures, and real pinned repository snapshots. Preserve raw measurements and
source/toolchain identities; profile before changing read or matching strategies.
