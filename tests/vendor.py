#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Vendor verification remains enforcing under Python optimization."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='search vendor ') as temporary:
    dest=Path(temporary)
    shutil.copytree(root/'vendor',dest/'vendor',ignore=shutil.ignore_patterns('__pycache__'))
    (dest/'scripts').mkdir()
    shutil.copy(root/'scripts/check-vendor.py',dest/'scripts/check-vendor.py')
    shutil.copy(root/'THIRD_PARTY_NOTICES.md',dest/'THIRD_PARTY_NOTICES.md')
    def verify(success):
        result=subprocess.run([sys.executable,str(dest/'scripts/check-vendor.py')],env={**os.environ,'PYTHONOPTIMIZE':'1'},capture_output=True)
        if (result.returncode==0)!=success:
            raise RuntimeError(result.stdout+result.stderr)
    verify(True)
    source=dest/'vendor/goose-regex/regex.goose'
    original=source.read_bytes()
    source.write_bytes(original+b'\n// modified\n')
    verify(False)
    source.write_bytes(original)
    extra=source.parent/'injected.goose'
    extra.write_text('import std;')
    verify(False)
    extra.unlink()
    source.unlink()
    source.symlink_to(root/'vendor/goose-regex/regex.goose')
    verify(False)
print('Optimized-Python vendor tampering/unrecorded-source/symlink regressions passed')
