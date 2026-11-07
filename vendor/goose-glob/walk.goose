// SPDX-License-Identifier: Apache-2.0
namespace glob_walk;
import std;
import glob;
import fs;

struct Options {
    hidden:           bool = false,
    max_depth:        i64  = 32,
    max_entries:      i64  = 10000,
    max_results:      i64  = 1000,
    max_result_bytes: i64  = 1048576,
    work_limit:       i64  = 1048576,
}
const MAX_QUEUED_PATH_BYTES = 16777216;
struct Pending { start: i64, end: i64, depth: i64 }
root struct State { visited: i64 = 0, result_bytes: i64 = 0, budget: glob::Budget, pending: Pending[>..], paths: u8[>..] }
fn from_fs(status: fs::Status) -> glob::Status {
    if status.error == fs::Error.None { return glob::Status { .. }; }
    return glob::Status {
        error:    if status.error == fs::Error.LimitExceeded { glob::Error.LimitExceeded } else { glob::Error.Filesystem },
        os_error: status.os_error,
    };
}
fn scan_directory(root: fs::Root, prefix: const u8[:], pattern: glob::Pattern&, excludes: (glob::Pattern&)[:],
                  depth: i64, options: Options, state: State&, out: u8[][>..]&) -> glob::Status {
    let directory, opened = fs::open_root_at(root, if prefix.len == 0 { "." } else { prefix });
    if opened.error != fs::Error.None { return from_fs(opened); }
    let result = scan_open_directory(directory, prefix, pattern, excludes, depth, options, state, out);
    let closed = fs::close_root(directory);
    if result.error != glob::Error.None { return result; }
    return from_fs(closed);
}
fn scan_open_directory(root: fs::Root, prefix: const u8[:], pattern: glob::Pattern&, excludes: (glob::Pattern&)[:],
         depth: i64, options: Options, state: State&, out: u8[][>..]&) -> glob::Status {
    var entries: fs::Entry[>..] = [];
    var listed = fs::list_sorted_at(root, ".", fs::ListOptions { byte_limit: 4096, entry_limit: 4096, .. }, entries);
    if listed.error == fs::Error.LimitExceeded {
        // Small directories avoid clearing 64 KiB; retry larger listings with
        // the original byte/entry caps and the backend's adaptive staging.
        listed = fs::list_sorted_at(root, ".", fs::ListOptions { byte_limit: 1048576, entry_limit: 4096, adaptive: true }, entries);
    }
    if listed.error != fs::Error.None { return from_fs(listed); }
    for entry in entries {
        if state.visited == options.max_entries { return glob::Status { error: glob::Error.LimitExceeded }; }
        state.visited++;
        let name = entry.name;
        if !options.hidden && name[0] == '.' { continue; }
        let path = if prefix.len == 0 { str(name) } else { str(prefix, "/", name) };
        var parts: glob::Span[>..] = [];
        if !glob::split(path, parts) { return glob::Status { error: glob::Error.LimitExceeded }; }
        var excluded = false;
        for exclude in excludes {
            if glob::match_with_budget(exclude, path, parts, true, state.budget) { excluded = true; }
            if state.budget.exhausted { return glob::Status { error: glob::Error.LimitExceeded }; }
            if excluded { break; }
        }
        if excluded { continue; }
        // Listing types are already observations of the entries, not a snapshot.
        // Filesystems without d_type need a no-follow metadata fallback.
        var kind = entry.kind;
        if kind == fs::Kind.Unknown {
            var metadata = fs::Metadata { .. };
            let inspected = fs::metadata_at(root, name, metadata);
            if inspected.error != fs::Error.None { return from_fs(inspected); }
            kind = metadata.kind;
        }
        if kind == fs::Kind.Directory {
            // Root entries have depth 1. Hitting the cap fails, never truncates.
            if depth == options.max_depth { return glob::Status { error: glob::Error.LimitExceeded }; }
            if path.len > MAX_QUEUED_PATH_BYTES - state.paths.len { return glob::Status { error: glob::Error.LimitExceeded }; }
            let start = state.paths.len;
            state.paths.append(path);
            state.pending.push(Pending { start: start, end: state.paths.len, depth: depth + 1 });
        } else if kind == fs::Kind.File {
            let matched = glob::match_with_budget(pattern, path, parts, options.hidden, state.budget);
            if state.budget.exhausted { return glob::Status { error: glob::Error.LimitExceeded }; }
            if matched {
                if out.len == options.max_results || path.len > options.max_result_bytes - state.result_bytes {
                    return glob::Status { error: glob::Error.LimitExceeded };
                }
                state.result_bytes += path.len; out.push(str(path));
            }
        }
    }
    return glob::Status { .. };
}
// Replaces out. Pattern/exclusions/root may borrow out: stage before clearing it.
fn collect(root_path: const u8[:], pattern: const u8[:], excludes: const (const u8[:])[:],
           options: Options, out: u8[][>..]&) -> glob::Status {
    var staged: u8[][>..] = [];
    let result = collect_into(root_path, pattern, excludes, options, staged);
    out.clear();
    if result.error == glob::Error.None { for path in staged { out.push(str(path)); } }
    return result;
}
fn collect_into(root_path: const u8[:], pattern: const u8[:], excludes: const (const u8[:])[:],
                options: Options, out: u8[][>..]&) -> glob::Status {
    if options.max_depth < 1 || options.max_depth > 64 || options.max_entries < 0 || options.max_entries > 65536 ||
       options.max_results < 0 || options.max_results > 65536 || options.max_result_bytes < 0 || options.max_result_bytes > 16777216 ||
       options.work_limit < 1 || options.work_limit > glob::MAX_WORK || excludes.len > 64 {
        return glob::Status { error: glob::Error.InvalidOptions };
    }
    let inclusion, checked = glob::compile(pattern);
    if checked.error != glob::Error.None { return checked; }
    var compiled_excludes: glob::Pattern[>..]    = [];
    var exclude_refs:      (glob::Pattern&)[>..] = [];
    for exclude in excludes {
        let compiled, check = glob::compile(exclude);
        if check.error != glob::Error.None { return check; }
        let item .= compiled_excludes.push(copy(compiled)); exclude_refs.push(item);
    }
    let context, created = fs::create_context();
    if created.error != fs::Error.None { return from_fs(created); }
    let root, opened = fs::open_root(context, root_path);
    if opened.error != fs::Error.None { let _ = fs::close_context(context); return from_fs(opened); }
    var state = State { budget: glob::Budget { remaining: options.work_limit }, pending: [], paths: [] };
    state.pending.push(Pending { start: 0, end: 0, depth: 1 });
    var at = 0;
    var result = glob::Status { .. };
    while at < state.pending.len {
        // Copy before growing pending; no borrowed queue element crosses a push.
        let item = state.pending[at];
        let prefix = str(state.paths[item.start..item.end]);
        let depth = item.depth;
        result = scan_directory(root, prefix, inclusion, exclude_refs, depth, options, state, out);
        if result.error != glob::Error.None { break; }
        at++;
    }
    let closed = fs::close_root(root);
    let released = fs::close_context(context);
    if result.error != glob::Error.None { return result; }
    if closed.error != fs::Error.None { return from_fs(closed); }
    return from_fs(released);
}
