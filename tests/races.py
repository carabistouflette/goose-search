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
        assert result.returncode==2,(kind,result.returncode,result.stderr)
        assert result.stdout==b'',(kind,result.stdout)
        assert b'cannot read selected regular file: b' in result.stderr,(kind,result.stderr)
print('Selected-file disappearance, FIFO replacement and symlink replacement rejected without output')
