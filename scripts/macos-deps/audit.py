#!/usr/bin/env python3
"""Reject bundled Mach-O files with incompatible targets or external dependencies.

This checks deployment metadata, architecture, dependency resolution and known
unsupported imports. It cannot replace launching the app on the oldest OS.
"""
import argparse
import re
import plistlib
import subprocess
from pathlib import Path

MAGICS = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xfe\xed\xfa\xce', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca', b'\xca\xfe\xba\xbf'}


def version(value):
    """Compare deployment versions independently of optional patch components."""
    parts = tuple(map(int, value.split('.')))
    return parts + (0,) * (3 - len(parts))


def inspect_loads(text):
    """Extract minimum OS versions, dependencies and rpaths from otool output."""
    minimums, dependencies, rpaths = [], [], []
    for block in re.split(r'\nLoad command \d+\n', text):
        if re.search(r'cmd LC_(BUILD_VERSION|VERSION_MIN_MACOSX)\b', block):
            match = re.search(r'\b(?:minos|version) (\d+(?:\.\d+)+)', block)
            if match:
                minimums.append(match[1])
        if re.search(r'cmd LC_(?:LOAD|LOAD_WEAK|REEXPORT|LOAD_UPWARD)_DYLIB\b', block):
            dependencies.append(re.search(r'\bname (.+) \(offset', block)[1])
        if 'cmd LC_RPATH' in block:
            rpaths.append(re.search(r'\bpath (.+) \(offset', block)[1])
    return minimums, dependencies, rpaths


def system_path(value):
    """Identify libraries supplied by macOS rather than third-party prefixes."""
    return value.startswith(('/usr/lib/', '/System/Library/'))


def resolve_dependency(value, binary, app, rpaths):
    """Resolve dyld path tokens and reject references outside the installed app."""
    if system_path(value):
        return True
    executable = app / 'Contents/MacOS'

    def expand(path):
        return Path(path.replace('@loader_path', str(binary.parent)).replace('@executable_path', str(executable)))

    candidates = []
    if value.startswith('@rpath/'):
        candidates = [expand(r) / value[len('@rpath/'):] for r in rpaths]
    else:
        candidates = [expand(value)]
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved.is_relative_to(app.resolve()) and resolved.is_file():
            return True
    return False


def audit(app, target, arch):
    """Return compatibility errors across every bundled Mach-O architecture."""
    errors, binaries = [], []
    for path in sorted(app.rglob('*')):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open('rb') as stream:
            magic = stream.read(4)
        if magic in MAGICS:
            binaries.append(path)
    if not binaries:
        return ['No Mach-O binaries found.']
    with (app / 'Contents/Info.plist').open('rb') as stream:
        executable_name = plistlib.load(stream)['CFBundleExecutable']
    executable = app / 'Contents/MacOS' / executable_name
    inherited = []
    if executable.exists():
        output = subprocess.check_output(['/usr/bin/otool', '-l', str(executable)], text=True)
        inherited = [r.replace('@loader_path', str(executable.parent)) for r in inspect_loads(output)[2]]
    for binary in binaries:
        relative = binary.relative_to(app)
        architectures = subprocess.check_output(['/usr/bin/lipo', '-archs', str(binary)], text=True).split()
        if arch not in architectures:
            errors.append(f'{relative}: missing {arch}')
        for architecture in architectures:
            output = subprocess.check_output(['/usr/bin/otool', '-arch', architecture, '-l', str(binary)], text=True)
            minimums, dependencies, rpaths = inspect_loads(output)
            if not minimums or any(version(v) > version(target) for v in minimums):
                errors.append(f'{relative} ({architecture}): minimum OS {minimums}, target {target}')
            for dependency in dependencies:
                if not resolve_dependency(dependency, binary, app, rpaths + inherited):
                    errors.append(f'{relative}: unresolved/external dependency {dependency}')
            for rpath in rpaths:
                if rpath.startswith(('/opt/homebrew', '/usr/local', '/opt/local')):
                    errors.append(f'{relative}: external rpath {rpath}')
            # Detect the actual startup failure even if somebody rewrites the minOS metadata.
            symbols = subprocess.check_output(['/usr/bin/nm', '-arch', architecture, '-m', '-u', str(binary)], text=True)
            if version(target) < version('27.0'):
                for line in symbols.splitlines():
                    if re.search(r'\b_pipe2\b', line) and 'weak' not in line:
                        errors.append(f'{relative}: imports macOS 27 pipe2')
    print(f'Checked {len(binaries)} Mach-O files; target macOS {target} / {arch}.')
    return errors


def main():
    """Audit an installed app before signing or notarization."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('--target', default='14.4')
    parser.add_argument('--arch', default='arm64')
    args = parser.parse_args()
    errors = audit(args.app.resolve(), args.target, args.arch)
    for error in errors:
        print(error)
    raise SystemExit(bool(errors))


if __name__ == '__main__':
    main()
