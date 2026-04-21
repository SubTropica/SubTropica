#!/usr/bin/env python3
"""Merge Mathematica-computed normalized symbol fields into entry.json files.

Reads ui/_norm_fields.json (output of recompute-normalized-symbols.wl)
and merges the fields into the corresponding library-bundled/ entry.json files.
Then rebuilds library.json via wolframscript.
"""

import json
import os
import sys

LIBRARY_DIR = 'library-bundled'
FIELDS_FILE = 'ui/_norm_fields.json'

def main():
    with open(FIELDS_FILE) as f:
        norm_fields = json.load(f)

    updated = 0
    for key, fields in norm_fields.items():
        # key format: "relative/path/entry.json:result_index"
        parts = key.rsplit(':', 1)
        if len(parts) != 2:
            print(f'Skipping bad key: {key}', file=sys.stderr)
            continue
        rel_path, idx_str = parts
        idx = int(idx_str)

        entry_path = os.path.join(LIBRARY_DIR, rel_path)
        if not os.path.exists(entry_path):
            print(f'Missing: {entry_path}', file=sys.stderr)
            continue

        with open(entry_path) as f:
            entry = json.load(f)

        results = entry.get('Results', [])
        if idx >= len(results):
            print(f'Index {idx} out of range for {entry_path}', file=sys.stderr)
            continue

        # Merge normalization fields into the result
        for field_name in ('normalizedSymbolTeX', 'wDefinitions', 'normalizedAlphabet',
                           'symbolScale', 'normalizedSymbolTerms'):
            if field_name in fields:
                results[idx][field_name] = fields[field_name]

        with open(entry_path, 'w') as f:
            json.dump(entry, f, indent=2, ensure_ascii=False)
            f.write('\n')
        updated += 1

    print(f'Updated {updated} entries')

if __name__ == '__main__':
    main()
