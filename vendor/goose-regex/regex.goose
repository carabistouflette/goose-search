// SPDX-License-Identifier: Apache-2.0
namespace regex;
import std;

enum Error { None, InvalidInput, InvalidHandle, InvalidPattern, LimitExceeded, OutOfMemory, BackendFailure, Busy }
// Copies alias one resource. Always close a successfully compiled pattern.
struct Pattern { token: i64 = 0, pairs: i64 = 0 }
struct Options {
    input_limit:  i64 = 1048576,
    memory_limit: i64 = 8388608,
    work_limit:   i64 = 1000000,
    depth_limit:  i64 = 1000,
}
struct Status {
    error:        Error = Error.None,
    backend_code: i64   = 0,
    error_offset: i64   = -1,
    matched:      bool  = false,
    pairs:        i64   = 0,
}
extern fn native_compile(pattern: const u8[:], config: i64[:], meta: i64[:]) -> i64 = "gr_compile";
extern fn native_find(token: i64, subject: const u8[:], start: i64, offsets: i64[:], meta: i64[:]) -> i64 = "gr_find";
extern fn native_close(token: i64) -> i64 = "gr_close";
fn error(code: i64) -> Error {
    if code == 0 { return Error.None; }
    if code == 1 { return Error.InvalidInput; }
    if code == 2 { return Error.InvalidHandle; }
    if code == 3 { return Error.InvalidPattern; }
    if code == 4 { return Error.LimitExceeded; }
    if code == 5 { return Error.OutOfMemory; }
    if code == 7 { return Error.Busy; }
    return Error.BackendFailure;
}
fn compile(pattern: const u8[:], options: const Options&) -> Pattern, Status {
    var config: i64[4] = [options.input_limit, options.memory_limit, options.work_limit, options.depth_limit];
    var meta:   i64[4] = [0, 0, 0, -1];
    let code = native_compile(pattern, config, meta);
    return Pattern { token: meta[0], pairs: meta[1] }, Status { error: error(code), backend_code: meta[2], error_offset: meta[3], .. };
}
// Supply at least 2*pattern.pairs and at most 128 integers. Unmatched: -1,-1.
fn find(pattern: Pattern, subject: const u8[:], start: i64, offsets: i64[:]) -> Status {
    var meta: i64[3] = [0; 3];
    let code = native_find(pattern.token, subject, start, offsets, meta);
    return Status { error: error(code), backend_code: meta[2], matched: meta[0] == 1, pairs: meta[1], .. };
}
fn close(pattern: Pattern) -> Error { return error(native_close(pattern.token)); }
