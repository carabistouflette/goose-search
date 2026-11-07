#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Content-address a test-only file replacement compiler/link wrapper."""
import argparse
import hashlib
from pathlib import Path
import shlex
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', required=True)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
compiler = Path(shutil.which(args.cc) or args.cc).resolve(strict=True)
compile_flags = '-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' if args.sanitize else ''
link_flags = compile_flags+' -no-pie' if args.sanitize else ''
text = f'''#!/bin/sh
for argument do
    if [ "$argument" = -c ]; then
        exec {shlex.quote(str(compiler))} {compile_flags} "$@"
    fi
done
exec {shlex.quote(str(compiler))} {link_flags} "$@" {shlex.quote(str(root/'tests/race.c'))} -Wl,--wrap=openat
'''
digest = hashlib.sha256(Path(__file__).read_bytes()+compiler.read_bytes()+(root/'tests/race.c').read_bytes()+text.encode()).hexdigest()
target = root/'.build'/('race-'+digest)/'cc'
target.parent.mkdir(parents=True, exist_ok=True)
if target.exists():
    if target.read_text() != text:
        raise SystemExit('content-addressed compiler wrapper differs')
else:
    target.write_text(text)
    target.chmod(0o755)
print(target)
