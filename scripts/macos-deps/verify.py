#!/usr/bin/env python3
"""Verify the integrity and identity of the local macOS release dependency prefix."""
import argparse
import json
import os
from pathlib import Path
from build import digest


def main():
    """Reject incomplete, relocated or modified release dependencies."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    args = parser.parse_args()
    prefix = args.prefix.resolve()
    manifest = json.loads((prefix / 'manifest.json').read_text())
    if manifest['prefix'] != str(prefix) or manifest['deployment_target'] != '14.4' or manifest['architecture'] != 'arm64':
        parser.error('Dependency prefix, architecture or deployment target does not match.')
    if 'ffmpeg_recipe' not in manifest:
        parser.error('Run scripts/macos-deps/prepare.py to include the patched FFmpeg bundle.')
    for relative, expected in manifest['files'].items():
        path = prefix / relative
        if not path.is_file() or digest(path) != expected:
            parser.error(f'Dependency checksum mismatch: {relative}')
    for relative, expected in manifest['symlinks'].items():
        path = prefix / relative
        if not path.is_symlink() or os.readlink(path) != expected:
            parser.error(f'Dependency symlink mismatch: {relative}')
    actual = {str(p.relative_to(prefix)) for p in prefix.rglob('*') if p.is_file() and not p.is_symlink() and p != prefix / 'manifest.json'}
    if actual != set(manifest['files']):
        parser.error('Unexpected or missing files in dependency prefix.')
    print(f'Verified {len(manifest["files"])} dependency files.')


if __name__ == '__main__':
    main()
