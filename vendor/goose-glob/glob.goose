// SPDX-License-Identifier: Apache-2.0
namespace glob;
import std;

const MAX_PATTERN_BYTES = 1024;
const MAX_PATH_BYTES = 4095;
const MAX_COMPONENTS = 128;
const MAX_WORK = 16777216;
const PATH_SEARCH_THRESHOLD = 32;
enum Error { None, InvalidPattern, InvalidPath, InvalidOptions, LimitExceeded, Filesystem }
struct Status { error: Error = Error.None, os_error: i64 = 0 }
struct Options { hidden: bool = false, work_limit: i64 = 1048576 }
struct Span { start: i64, end: i64 }
enum ComponentKind { General, Literal, Prefix, Suffix, Globstar, Stars }
struct Part { start: i64, end: i64, kind: ComponentKind }
struct Pattern { source: u8[], parts: Part[], globstars: i64 = 0 }
struct Budget { remaining: i64, exhausted: bool = false }

fn spend(budget: Budget&) -> bool {
    if budget.remaining == 0 { budget.exhausted = true; return false; }
    budget.remaining--; return true;
}
let PATH_END = byte_set("/\0");
let PATH_SLASH = byte_set("/");
let PATH_NUL = byte_set("\0");
let PATTERN_META = byte_set("/\0\\[?*");
inline fn valid_component_span(path: const u8[:], start: i64, end: i64) -> bool {
    let size = end - start;
    return size > 0 && !(size == 1 && path[start] == '.') &&
           !(size == 2 && path[start] == '.' && path[start + 1] == '.');
}
// The byte-search setup pays off on longer paths. Retain spans only when
// matching needs them; rejected prefixes still validate every component.
fn scan_components(path: const u8[:], out: Span[>..]&, retain: bool) -> i64 {
    if path.len == 0 || path.len > MAX_PATH_BYTES || find_any(path, PATH_NUL) >= 0 { return 0; }
    var start = 0; var count = 0;
    while start < path.len {
        let offset = find_any(path[start..], PATH_SLASH);
        let end = if offset < 0 { path.len } else { start + offset };
        if !valid_component_span(path, start, end) || count == MAX_COMPONENTS { return 0; }
        count++;
        if retain { out.push(Span { start: start, end: end }); }
        if offset < 0 { return count; }
        start = end + 1;
    }
    return 0;
}
fn split(path: const u8[:], out: Span[>..]&) -> bool {
    out.clear();
    if path.len == 0 || path.len > MAX_PATH_BYTES { return false; }
    if path.len >= PATH_SEARCH_THRESHOLD { return scan_components(path, out, true) > 0; }
    var start = 0;
    for c, i in path {
        if c == 0 { return false; }
        if c == '/' {
            if !valid_component_span(path, start, i) || out.len == MAX_COMPONENTS { return false; }
            out.push(Span { start: start, end: i }); start = i + 1;
        }
    }
    if !valid_component_span(path, start, path.len) || out.len == MAX_COMPONENTS { return false; }
    out.push(Span { start: start, end: path.len }); return true;
}
// Returns the closing bracket, or -1 for an empty/malformed class.
fn class_end(pattern: const u8[:], start: i64) -> i64 {
    var at = start + 1;
    if at < pattern.len && (pattern[at] == '!' || pattern[at] == '^') { at++; }
    let first = at;
    while at < pattern.len && pattern[at] != ']' {
        // Escapes inside classes are deliberately unsupported.
        if pattern[at] == '\\' || pattern[at] == '/' || pattern[at] == 0 { return -1; }
        if at + 2 < pattern.len && pattern[at + 1] == '-' && pattern[at + 2] != ']' {
            if pattern[at] > pattern[at + 2] || pattern[at + 2] == '\\' || pattern[at + 2] == '/' || pattern[at + 2] == 0 { return -1; }
            at += 3;
        } else { at++; }
    }
    return if at < pattern.len && at > first { at } else { -1 };
}
// Validate and classify directly into final metadata, without temporary spans
// or rescanning every component to count its wildcard syntax.
fn scan_pattern(pattern: const u8[:], out: Part[>..]&) -> i64 {
    if pattern.len == 0 || pattern.len > MAX_PATTERN_BYTES { return -1; }
    var at = 0; var globstars = 0;
    while at < pattern.len {
        let start = at;
        var stars = 0; var general = false; var globstar = false;
        while at < pattern.len {
            while at < pattern.len && !PATTERN_META.members[pattern[at]] { at++; }
            if at == pattern.len || pattern[at] == '/' { break; }
            let c = pattern[at];
            if c == 0 { return -1; }
            if c == '\\' {
                general = true; at++;
                if at == pattern.len || pattern[at] == '/' || pattern[at] == 0 { return -1; }
            } else if c == '[' {
                general = true; at = class_end(pattern, at);
                if at < 0 { return -1; }
            } else if c == '?' { general = true; }
            else if c == '*' {
                stars++;
                if at + 1 < pattern.len && pattern[at + 1] == '*' {
                    if at != start || (at + 2 < pattern.len && pattern[at + 2] != '/') { return -1; }
                    globstar = true; at++;
                }
            }
            at++;
        }
        if !valid_component_span(pattern, start, at) || out.len == MAX_COMPONENTS { return -1; }
        let kind = if globstar { ComponentKind.Globstar } else if general { ComponentKind.General }
                   else if stars == 0 { ComponentKind.Literal }
                   else if stars == 1 && pattern[start] == '*' { ComponentKind.Suffix }
                   else if stars == 1 && pattern[at - 1] == '*' { ComponentKind.Prefix }
                   else { ComponentKind.Stars };
        if globstar { globstars++; }
        out.push(Part { start: start, end: at, kind: kind });
        if at == pattern.len { return globstars; }
        at++;
    }
    return -1;
}
inline fn parse_pattern(pattern: const u8[:], out: Part[>..]&) -> i64 {
    out.clear();
    let globstars = scan_pattern(pattern, out);
    if globstars < 0 { out.clear(); }
    return globstars;
}
fn validate(pattern: const u8[:]) -> Status {
    var parts: Part[>..] = [];
    return Status { error: if parse_pattern(pattern, parts) >= 0 { Error.None } else { Error.InvalidPattern } };
}
fn owned<T>(source: const T[:]) -> T[] {
    var out: T[>..] = []; out.append(source); return out;
}
fn compile(pattern: const u8[:]) -> Pattern, Status {
    var parts: Part[>..] = [];
    let globstars = parse_pattern(pattern, parts);
    if globstars < 0 { return Pattern { .. }, Status { error: Error.InvalidPattern }; }
    return Pattern { source: owned(pattern), parts: owned(parts), globstars: globstars }, Status { .. };
}
// Pattern syntax is already validated: decode the class once while matching.
fn match_class(pattern: const u8[:], start: i64, c: u8, budget: Budget&) -> i64, bool {
    var at = start + 1;
    let negate = pattern[at] == '!' || pattern[at] == '^';
    if negate { at++; }
    var found = false;
    while at < pattern.len && pattern[at] != ']' {
        if at + 2 < pattern.len && pattern[at + 1] == '-' && pattern[at + 2] != ']' {
            if budget.remaining < 3 { budget.exhausted = true; return at, false; }
            budget.remaining -= 3;
            if c >= pattern[at] && c <= pattern[at + 2] { found = true; }
            at += 3;
        } else {
            if !spend(budget) { return at, false; }
            if c == pattern[at] { found = true; }
            at++;
        }
    }
    return at, if negate { !found } else { found };
}
// Greedy component matching with one saved star. No recursive backtracking.
fn component_matches(pattern: const u8[:], name: const u8[:], hidden: bool, budget: Budget&) -> bool {
    let explicit_dot = pattern[0] == '.' || (pattern.len >= 2 && pattern[0] == '\\' && pattern[1] == '.');
    if !hidden && name[0] == '.' && !explicit_dot { return false; }
    let last = pattern[pattern.len - 1];
    if last != '*' && last != '?' && last != ']' && name[name.len - 1] != last { return false; }
    var p = 0; var n = 0; var star = -1; var retry = 0;
    while n < name.len {
        if !spend(budget) { return false; }
        if p < pattern.len && pattern[p] == '*' { star = p; p++; retry = n; continue; }
        var next = p + 1;
        var matched = false;
        if p < pattern.len {
            let c = pattern[p];
            if c == '?' { matched = true; }
            else if c == '\\' { matched = pattern[p + 1] == name[n]; next = p + 2; }
            else if c == '[' {
                let end, hit = match_class(pattern, p, name[n], budget);
                if budget.exhausted { return false; }
                matched = hit; next = end + 1;
            } else { matched = c == name[n]; }
        }
        if matched { p = next; n++; }
        else if star >= 0 { retry++; n = retry; p = star + 1; }
        else { return false; }
    }
    if p < pattern.len && pattern[p] == '*' { p++; }
    return p == pattern.len;
}
// Search only as many candidate bytes as the remaining work budget permits.
// Count actual prefix comparisons, so cheap first-byte misses do not consume
// the worst-case length of a large literal chunk.
fn find_chunk(source: const u8[:], chunk: const u8[:], budget: Budget&) -> i64 {
    let positions = source.len - chunk.len + 1;
    var at = 0;
    while at < positions {
        let allowed = min(positions - at, budget.remaining);
        if allowed == 0 { budget.exhausted = true; return -1; }
        let found = position(source[at..at + allowed], chunk[0]);
        budget.remaining -= if found < 0 { allowed } else { found + 1 };
        if found < 0 {
            if allowed < positions - at { budget.exhausted = true; }
            return -1;
        }
        at += found;
        var matched = 1;
        while matched < chunk.len {
            if !spend(budget) { return -1; }
            if source[at + matched] != chunk[matched] { break; }
            matched++;
        }
        if matched == chunk.len { return at; }
        at++;
    }
    return -1;
}
// Literal-only star patterns search whole chunks rather than retrying one byte
// at a time. The search window and each comparison are bounded by fuel.
fn star_matches(pattern: const u8[:], name: const u8[:], budget: Budget&) -> bool {
    if pattern[pattern.len - 1] != '*' && name[name.len - 1] != pattern[pattern.len - 1] { return false; }
    var p = 0; var n = 0;
    while p < pattern.len {
        if pattern[p] == '*' { p++; continue; }
        let start = p;
        while p < pattern.len && pattern[p] != '*' { p++; }
        let chunk = pattern[start..p];
        if chunk.len > name.len - n { return false; }
        if start == 0 || p == pattern.len {
            if budget.remaining < chunk.len { budget.exhausted = true; return false; }
            budget.remaining -= chunk.len;
            if start == 0 {
                if !starts_with(name, chunk) { return false; }
                n = chunk.len;
            } else { return ends_with(name[n..], chunk); }
        } else {
            let found = find_chunk(name[n..], chunk, budget);
            if found < 0 { return false; }
            n += found + chunk.len;
        }
    }
    return true;
}
// Literal components and single edge-stars use bounded slice comparisons.
fn part_matches(pattern: const u8[:], name: const u8[:], kind: ComponentKind, hidden: bool, budget: Budget&) -> bool {
    if kind == ComponentKind.General { return component_matches(pattern, name, hidden, budget); }
    if !hidden && name[0] == '.' && pattern[0] != '.' { return false; }
    if kind == ComponentKind.Stars { return star_matches(pattern, name, budget); }
    let work = if pattern.len < name.len { pattern.len } else { name.len };
    if budget.remaining < work { budget.exhausted = true; return false; }
    budget.remaining -= work;
    if kind == ComponentKind.Literal { return pattern == name; }
    if kind == ComponentKind.Prefix { return starts_with(name, pattern[..pattern.len - 1]); }
    return ends_with(name, pattern[1..]);
}
fn match_part(pattern: const u8[:], path: const u8[:], part: Part, name: Span, hidden: bool, budget: Budget&) -> bool {
    return spend(budget) && part_matches(pattern[part.start..part.end], path[name.start..name.end], part.kind, hidden, budget);
}
// Match fixed components from the two ends when there is at most one **.
// Multiple ** components use a bounded greedy scan with one retry point.
fn match_parts(pattern: const u8[:], path: const u8[:], patterns: Part[:], names: Span[:], globstars: i64, hidden: bool, budget: Budget&) -> bool {
    if globstars == 0 {
        if patterns.len != names.len { return false; }
        for part, i in patterns {
            if !match_part(pattern, path, part, names[i], hidden, budget) { return false; }
        }
        return true;
    }
    if globstars == 1 {
        if names.len < patterns.len - 1 { return false; }
        var star = 0;
        while patterns[star].kind != ComponentKind.Globstar {
            if !match_part(pattern, path, patterns[star], names[star], hidden, budget) { return false; }
            star++;
        }
        let consumed_end = names.len - (patterns.len - star - 1);
        for i in star..consumed_end {
            if !spend(budget) { return false; }
            if !hidden && path[names[i].start] == '.' { return false; }
        }
        for i in star + 1..patterns.len {
            if !match_part(pattern, path, patterns[i], names[consumed_end + i - star - 1], hidden, budget) { return false; }
        }
        return true;
    }
    var p = 0; var n = 0; var star = -1; var retry = 0;
    while n < names.len {
        if !spend(budget) { return false; }
        if p < patterns.len && patterns[p].kind == ComponentKind.Globstar {
            star = p; p++; retry = n; continue;
        }
        if p < patterns.len && match_part(pattern, path, patterns[p], names[n], hidden, budget) { p++; n++; }
        else {
            if budget.exhausted || star < 0 || retry == names.len || (!hidden && path[names[retry].start] == '.') { return false; }
            retry++; n = retry; p = star + 1;
        }
    }
    while p < patterns.len && patterns[p].kind == ComponentKind.Globstar { p++; }
    return p == patterns.len;
}
// Internal traversal entry point reuses the collector's validated spans and
// shares one work budget across all paths.
fn match_with_budget(pattern: Pattern&, path: const u8[:], names: Span[:], hidden: bool, budget: Budget&) -> bool {
    return match_parts(pattern.source, path, pattern.parts, names, pattern.globstars, hidden, budget);
}
fn valid_options(options: Options) -> bool { options.work_limit >= 1 && options.work_limit <= MAX_WORK }
inline fn match_result(pattern: const u8[:], parts: Part[:], globstars: i64, path: const u8[:], options: Options) -> bool, Status {
    // A validated all-literal pattern equal to its candidate proves candidate
    // validity too. Mismatches still validate the complete candidate path.
    if globstars == 0 && options.work_limit >= pattern.len + parts.len {
        var literal = true;
        for part in parts { if part.kind != ComponentKind.Literal { literal = false; break; } }
        if literal {
            if pattern == path { return true, Status { .. }; }
            var checked: Span[>..] = [];
            if !split(path, checked) { return false, Status { error: Error.InvalidPath }; }
            return false, Status { .. };
        }
    }
    // The common one-component case needs no temporary path-span array.
    if parts.len == 1 && globstars == 0 && path.len > 0 && path.len <= MAX_PATH_BYTES && find_any(path, PATH_END) < 0 {
        if !valid_component_span(path, 0, path.len) { return false, Status { error: Error.InvalidPath }; }
        var budget = Budget { remaining: options.work_limit };
        let part = parts[0];
        let matched = spend(budget) && part_matches(pattern[part.start..part.end], path, part.kind, options.hidden, budget);
        return matched && !budget.exhausted, Status { error: if budget.exhausted { Error.LimitExceeded } else { Error.None } };
    }
    var names: Span[>..] = [];
    if !split(path, names) { return false, Status { error: Error.InvalidPath }; }
    var budget = Budget { remaining: options.work_limit };
    let matched = match_parts(pattern, path, parts, names, globstars, options.hidden, budget);
    if budget.exhausted { return false, Status { error: Error.LimitExceeded }; }
    return matched, Status { .. };
}
inline fn matches(pattern: const u8[:], path: const u8[:], options: Options = Options { .. }) -> bool, Status {
    if !valid_options(options) { return false, Status { error: Error.InvalidOptions }; }
    var parts: Part[>..] = [];
    let globstars = parse_pattern(pattern, parts);
    if globstars < 0 { return false, Status { error: Error.InvalidPattern }; }
    return match_result(pattern, parts, globstars, path, options);
}
inline fn matches(pattern: Pattern&, path: const u8[:], options: Options = Options { .. }) -> bool, Status {
    if !valid_options(options) { return false, Status { error: Error.InvalidOptions }; }
    if pattern.parts.len == 0 { return false, Status { error: Error.InvalidPattern }; }
    // A literal prefix miss cannot exhaust this per-call budget. Long rejected
    // candidates can be fully validated without building unused span storage.
    if path.len >= PATH_SEARCH_THRESHOLD && pattern.parts.len > 1 && pattern.globstars <= 1 && pattern.parts[0].kind == ComponentKind.Literal {
        let first = pattern.parts[0]; let prefix = pattern.source[first.start..first.end];
        if options.work_limit >= prefix.len + 1 &&
           (path.len < prefix.len || path[..prefix.len] != prefix || (path.len > prefix.len && path[prefix.len] != '/')) {
            var checked: Span[>..] = [];
            if scan_components(path, checked, false) == 0 { return false, Status { error: Error.InvalidPath }; }
            return false, Status { .. };
        }
    }
    return match_result(pattern.source, pattern.parts, pattern.globstars, path, options);
}
