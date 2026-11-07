#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('--tool',required=True)
args=parser.parse_args()
tool=str(Path(args.tool).resolve())
for kind in ['gone','fifo','link']:
    with tempfile.TemporaryDirectory(prefix='search race ') as temporary:
        root=Path(temporary)
        (root/'a').write_bytes(b'hit')
        (root/'b').write_bytes(b'hit')
        result=subprocess.run([tool,'--','hit',str(root)],env={**os.environ,'SEARCH_RACE':kind},capture_output=True,timeout=5)
        if result.returncode!=2 or result.stdout!=b'' or b'cannot read selected regular file: b' not in result.stderr:
            raise RuntimeError((kind,result.returncode,result.stdout,result.stderr))
print('Selected-file disappearance, FIFO replacement and symlink replacement rejected without output')

for kind in ['grow','grow-binary','grow-large']:
    for initial in [b'',b'x']:
        with tempfile.TemporaryDirectory(prefix='search growth ') as temporary:
            root=Path(temporary)
            (root/'a').write_bytes(b'hit')
            (root/'b').write_bytes(initial)
            result=subprocess.run([tool,'--','hit',str(root)],env={**os.environ,'SEARCH_RACE':kind},capture_output=True,timeout=5)
            wanted=b'a:1:1:hit\n'+(b'b:1:1:hit longer\n' if kind=='grow' else b'')
            if kind=='grow-large':
                if result.returncode!=2 or result.stdout!=b'': raise RuntimeError((kind,result))
            elif result.returncode!=0 or result.stdout!=wanted: raise RuntimeError((kind,result))
print('Bounded growth retry, late-binary classification and oversized growth regressions passed')
