#!/usr/bin/env python3
"""Prepare and validate the complete release prefix, including patched FFmpeg."""
import hashlib
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from build import ROOT, digest
from audit import inspect_loads, version


def main():
    """Build missing dependencies, retain FFmpeg patches, and hash the final prefix."""
    work = ROOT / 'cmake-deps/macos'
    prefix = work / 'arm64'
    script = ROOT / 'third-party/ffmpeg-patches/build-bundle.sh'
    recipe = hashlib.sha256(script.read_bytes())
    for patch in sorted(script.parent.glob('build-deps/patches/FFmpeg/FFmpeg/videotoolbox/*.patch')):
        recipe.update(patch.read_bytes())
    stamp = work / 'ffmpeg-work/recipe.sha256'
    fingerprint = recipe.hexdigest()
    manifest_path = prefix / 'manifest.json'
    if manifest_path.exists():
        existing = json.loads(manifest_path.read_text())
        if 'ffmpeg_recipe' in existing:
            subprocess.run([sys.executable, str(Path(__file__).with_name('verify.py')), str(prefix)], check=True)
            if existing['ffmpeg_recipe'] == fingerprint:
                return
    subprocess.run([sys.executable, str(Path(__file__).with_name('build.py')), *sys.argv[1:]], check=True)
    if not stamp.exists() or stamp.read_text() != fingerprint or not (prefix / 'ffmpeg/lib/libavcodec.a').exists():
        env = os.environ.copy()
        for key in ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
                    'CPPFLAGS', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'PKG_CONFIG_PATH', 'SDKROOT'):
            env.pop(key, None)
        env.update(WORK=str(stamp.parent), MACOSX_DEPLOYMENT_TARGET='14.4')
        subprocess.run(['/bin/bash', str(script)], env=env, check=True)
        shutil.copytree(stamp.parent / 'bundle/ffmpeg', prefix / 'ffmpeg', dirs_exist_ok=True)
        stamp.write_text(fingerprint)
    for name in ('avcodec', 'avutil', 'swscale', 'x264', 'x265', 'SvtAv1Enc', 'cbs'):
        archive = prefix / 'ffmpeg/lib' / f'lib{name}.a'
        output = subprocess.check_output(['/usr/bin/otool', '-l', str(archive)], text=True)
        minimums = inspect_loads(output)[0]
        if not minimums or any(version(v) > version('14.4') for v in minimums):
            raise RuntimeError(f'Incompatible archive: {archive}: {set(minimums)}')
    manifest_path = prefix / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['ffmpeg_recipe'] = fingerprint
    manifest['files'] = {str(p.relative_to(prefix)): digest(p) for p in sorted(prefix.rglob('*'))
                         if p.is_file() and not p.is_symlink() and p != manifest_path}
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Release prefix ready: {prefix}')


if __name__ == '__main__':
    lock_path = ROOT / 'cmake-deps/macos/.prepare.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open('w') as lock_file:
        fcntl.flock(lock_file, fcntl.LOCK_EX)
        main()
