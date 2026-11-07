// SPDX-License-Identifier: Apache-2.0
namespace searching;
import std;
import fs;
import regex;
import glob;
import glob.walk;
const FILE_BYTES = 1048576;
const LINE_BYTES = 65536;
const TOTAL_BYTES = 67108864;
const MAX_CALLS = 1000000;
const MAX_MATCHES = 10000;
const OUTPUT_BYTES = 8388608;
enum Error { None, Collection, InvalidGlob, CollectionLimit, Open, Read, FileLimit, TotalLimit, LineLimit, CallLimit, MatchLimit, OutputLimit, Regex, Close }
struct Status { error: Error = Error.None, os_error: i64 = 0, path: u8[>..] }
struct Counts { bytes: i64 = 0, calls: i64 = 0, matches: i64 = 0 }
fn failure(error: Error, path: const u8[:] = "", os_error: i64 = 0) -> Status {
    return Status { error: error, os_error: os_error, path: str(path) };
}
fn escape(bytes: const u8[:], path: bool, out: u8[>..]&, limit: i64 = OUTPUT_BYTES) -> bool {
    let hex = "0123456789abcdef";
    for c in bytes {
        let encoded = c < 32 || c >= 127 || c == '\\' || (path && c == ':');
        let needed = if encoded { 4 } else { 1 };
        if needed > limit - out.len { return false; }
        if encoded { out.append("\\x"); out.push(hex[c >> 4]); out.push(hex[c & 15]); }
        else { out.push(c); }
    }
    return true;
}
fn line(pattern: regex::Pattern, bytes: const u8[:], name: const u8[:], escaped_name: const u8[:],
        number: i64, offsets: i64[:], counts: Counts&, out: u8[>..]&) -> Status {
    if bytes.len > LINE_BYTES { return failure(Error.LineLimit, name); }
    if counts.calls == MAX_CALLS { return failure(Error.CallLimit, name); }
    counts.calls++;
    let found = regex::find(pattern, bytes, 0, offsets);
    if found.error != regex::Error.None { return failure(Error.Regex, name); }
    if !found.matched { return Status { .. }; }
    if counts.matches == MAX_MATCHES { return failure(Error.MatchLimit, name); }
    let header = str(escaped_name, ":", number, ":", offsets[0] + 1, ":");
    if header.len + 1 > OUTPUT_BYTES - out.len { return failure(Error.OutputLimit, name); }
    out.append(header);
    if !escape(bytes, false, out, OUTPUT_BYTES - 1) { return failure(Error.OutputLimit, name); }
    out.push('\n'); counts.matches++;
    return Status { .. };
}
// Size a bounded read from metadata, then retry once if the file grew. Neither
// observation is a snapshot; the no-follow read still validates the opened file.
fn read(root: fs::Root, path: const u8[:], ceiling: i64, bytes: u8[>..]&) -> fs::Status {
    var metadata = fs::Metadata { .. };
    let inspected = fs::metadata_at(root, path, metadata);
    if inspected.error != fs::Error.None { bytes.clear(); return inspected; }
    if metadata.kind != fs::Kind.File { bytes.clear(); return fs::Status { error: fs::Error.NotRegular }; }
    let requested = if metadata.size < (ceiling as u64) { metadata.size as! i64 } else { ceiling };
    let result = fs::read_file_at(root, path, requested, bytes);
    if result.error == fs::Error.LimitExceeded && requested < ceiling {
        return fs::read_file_at(root, path, ceiling, bytes);
    }
    return result;
}
fn scan(pattern: regex::Pattern, root: fs::Root, paths: const u8[][:], binary: bool,
        counts: Counts&, out: u8[>..]&) -> Status {
    var bytes:   u8[>..]  = [];
    var offsets: i64[128] = [-1; 128];
    for path in paths {
        let remaining = TOTAL_BYTES - counts.bytes;
        let limit = min(FILE_BYTES, remaining);
        let read = read(root, path, limit, bytes);
        if read.error != fs::Error.None {
            if read.error == fs::Error.LimitExceeded { return failure(if remaining < FILE_BYTES { Error.TotalLimit } else { Error.FileLimit }, path); }
            return failure(Error.Read, path, read.os_error);
        }
        counts.bytes += bytes.len;
        var skip = false;
        if !binary { for c in bytes { if c == 0 { skip = true; break; } } }
        if skip { continue; }
        var escaped: u8[>..] = [];
        let encoded = escape(path, true, escaped);
        if !encoded { return failure(Error.OutputLimit, path); }
        var start:  i64 = 0;
        var number: i64 = 1;
        for c, at in bytes {
            if c != '\n' { continue; }
            let status = line(pattern, bytes[start..at], path, escaped, number, offsets[..pattern.pairs * 2], counts, out);
            if status.error != Error.None { return status; }
            start = at + 1; number++;
        }
        if start < bytes.len {
            let status = line(pattern, bytes[start..], path, escaped, number, offsets[..pattern.pairs * 2], counts, out);
            if status.error != Error.None { return status; }
        }
    }
    return Status { .. };
}
fn run(pattern: regex::Pattern, directory: const u8[:], inclusion: const u8[:], binary: bool,
       counts: Counts&, out: u8[>..]&) -> Status {
    var paths: u8[][>..] = [];
    let excludes: const u8[:][] = ["**/vendor", "**/target", "**/node_modules"];
    let collected = glob_walk::collect(directory, inclusion, excludes, glob_walk::Options { .. }, paths);
    if collected.error != glob::Error.None {
        let error = if collected.error == glob::Error.InvalidPattern { Error.InvalidGlob }
                    else if collected.error == glob::Error.LimitExceeded { Error.CollectionLimit }
                    else { Error.Collection };
        return failure(error, "", collected.os_error);
    }
    let context, created = fs::create_context();
    if created.error != fs::Error.None { return failure(Error.Open, "", created.os_error); }
    let root, opened = fs::open_root(context, directory);
    if opened.error != fs::Error.None { let _ = fs::close_context(context); return failure(Error.Open, "", opened.os_error); }
    let result = scan(pattern, root, paths, binary, counts, out);
    let closed = fs::close_root(root);
    let released = fs::close_context(context);
    if result.error != Error.None { return result; }
    if closed.error != fs::Error.None { return failure(Error.Close, "", closed.os_error); }
    if released.error != fs::Error.None { return failure(Error.Close, "", released.os_error); }
    return Status { .. };
}
fn message(error: Error) -> const u8[:] {
    if error == Error.InvalidGlob { return "invalid --glob pattern; use *, ?, character classes, or whole-component **"; }
    if error == Error.CollectionLimit { return "file collection limit exceeded; search a smaller root"; }
    if error == Error.Collection { return "file collection failed; check the root, directory permissions, and concurrent changes"; }
    if error == Error.Open { return "cannot open search root"; }
    if error == Error.Read { return "cannot read selected regular file"; }
    if error == Error.FileLimit { return "file byte limit exceeded"; }
    if error == Error.TotalLimit { return "total input byte limit exceeded"; }
    if error == Error.LineLimit { return "line byte limit exceeded"; }
    if error == Error.CallLimit { return "regex call limit exceeded"; }
    if error == Error.MatchLimit { return "matching line limit exceeded"; }
    if error == Error.OutputLimit { return "encoded output limit exceeded"; }
    if error == Error.Regex { return "regex search failed (work, depth, memory or backend error)"; }
    return "resource cleanup failed";
}
