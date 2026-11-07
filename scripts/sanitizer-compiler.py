#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Content-address a fixed sanitizer wrapper and the actual compiler binary."""
import argparse
import hashlib
from pathlib import Path
import shlex
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', required=True)
args = parser.parse_args()
compiler = Path(shutil.which(args.cc) or args.cc).resolve(strict=True)
text = '''#!/bin/sh
for argument do
    if [ "$argument" = -c ]; then
        exec COMPILER -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie "$@"
    fi
done
exec COMPILER -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie "$@"
'''.replace('COMPILER', shlex.quote(str(compiler)))
digest = hashlib.sha256(Path(__file__).read_bytes()+compiler.read_bytes()+text.encode()).hexdigest()
target = Path(__file__).resolve().parents[1]/'.build'/('sanitizer-'+digest)/'cc'
target.parent.mkdir(parents=True, exist_ok=True)
if target.exists():
    if target.read_text() != text:
        raise SystemExit('content-addressed compiler wrapper differs')
else:
    target.write_text(text)
    target.chmod(0o755)
print(target)
