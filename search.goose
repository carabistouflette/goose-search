// SPDX-License-Identifier: Apache-2.0
import regex from "./vendor/goose-regex";
import fs from "./vendor/goose-fs";
import glob from "./vendor/goose-glob";
import .src.searching;
import std;
import os;
extern fn checked_write(descriptor: i64, bytes: const u8[:]) -> i64 = "search_write";
fn diagnostic(message: const u8[:]) { let _ = checked_write(2, str("goose-search: ", message, "\n")); }
fn usage() -> const u8[:] { return "usage: goose-search [--glob PATTERN] [--binary skip|search] -- REGEX ABSOLUTE_ROOT\n"; }
fn execute(argv: u8[:][:]) -> i32 {
    if argv.len == 2 && argv[1] == "--help" { return if checked_write(1, usage()) == 1 { 0 } else { 2 }; }
    var inclusion: u8[>..] = str("**");
    var binary = false;
    var seen: i64 = 0;
    var at:   i64 = 1;
    while at < argv.len && argv[at] != "--" {
        let option = argv[at];
        if at + 1 >= argv.len { diagnostic("option requires a value"); return 2; }
        let value = argv[at + 1];
        if option == "--glob" && (seen & 1) == 0 && value.len > 0 && value.len <= 4096 { inclusion = str(value); seen |= 1; }
        else if option == "--binary" && (seen & 2) == 0 && (value == "skip" || value == "search") { binary = value == "search"; seen |= 2; }
        else { diagnostic("invalid or duplicate option"); return 2; }
        at += 2;
    }
    if at >= argv.len || argv.len - at != 3 { let _ = checked_write(2, usage()); return 2; }
    let options = regex::Options { input_limit: searching::LINE_BYTES, work_limit: 100000, .. };
    let pattern, compiled = regex::compile(argv[at + 1], options);
    if compiled.error != regex::Error.None { diagnostic(str("invalid regex or compile limit; byte offset=", compiled.error_offset)); return 2; }
    var out: u8[>..] = [];
    var counts = searching::Counts { .. };
    let status = searching::run(pattern, argv[at + 2], inclusion, binary, counts, out);
    let closed = regex::close(pattern);
    if status.error != searching::Error.None {
        var name: u8[>..] = [];
        let _ = searching::escape(status.path, true, name);
        diagnostic(str(searching::message(status.error), if name.len > 0 { ": " } else { "" }, name, "; errno=", status.os_error));
        return 2;
    }
    if closed != regex::Error.None { diagnostic("regex cleanup failed"); return 2; }
    if checked_write(1, out) != 1 { diagnostic("stdout write failed (a prefix may have been written)"); return 2; }
    return if counts.matches > 0 { 0 } else { 1 };
}
fn main() { let argv = args(); let code = execute(argv[..]); if code != 0 { exit(code); } }
