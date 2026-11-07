#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate vendored immutable snapshots; never read dirty sibling libraries."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root/'vendor/pins.json').read_text())
assert manifest['schema'] == 1
notices = (root/'THIRD_PARTY_NOTICES.md').read_text()
for item in manifest['libraries']:
    assert item['revision'] in notices
    assert Path(item['directory']).name == item['directory']
    for name, wanted in item['files'].items():
        path = Path(name)
        assert not path.is_absolute() and '..' not in path.parts
        assert hashlib.sha256((root/'vendor'/item['directory']/path).read_bytes()).hexdigest() == wanted, (item['directory'], name)
print('Immutable vendor source/license hashes verified')
