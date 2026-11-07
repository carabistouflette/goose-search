#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate vendored immutable snapshots; never read dirty sibling libraries."""
import hashlib
import json
from pathlib import Path
import re


def fail(message):
    raise SystemExit(message)


root = Path(__file__).resolve().parents[1]
manifest = json.loads((root/'vendor/pins.json').read_text())
if manifest.get('schema') != 1:
    fail('unsupported vendor manifest schema')
notices = (root/'THIRD_PARTY_NOTICES.md').read_text()
seen = set()
for item in manifest['libraries']:
    name = item['directory']
    if name in seen or name in ('.', '..') or len(Path(name).parts) != 1 or Path(name).is_absolute():
        fail('unsafe or duplicate vendor directory')
    seen.add(name)
    if not re.fullmatch('[0-9a-f]{40}', item['revision']) or item['revision'] not in notices:
        fail('vendor revision missing from notices')
    base = root/'vendor'/name
    if base.is_symlink():
        fail('symlinked vendor directory')
    for name, wanted in item['files'].items():
        path = Path(name)
        if path.is_absolute() or '..' in path.parts or str(path) != name:
            fail('unsafe vendor file path')
        target = base/path
        if any(parent.is_symlink() for parent in [target, *target.parents]):
            fail('symlinked vendor file/ancestor')
        if hashlib.sha256(target.read_bytes()).hexdigest() != wanted:
            fail(f'vendored bytes differ: {base.name}/{name}')
    if any(p.is_symlink() for p in base.rglob('*')):
        fail(f'symlinked vendor entry: {base.name}')
    actual = {p.relative_to(base).as_posix() for p in base.rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    if actual != set(item['files']):
        fail(f'unrecorded/missing vendor files: {base.name}')
print('Immutable vendor source/license hashes verified')
