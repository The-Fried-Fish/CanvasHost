#!/usr/bin/env python3
"""Build the locked macOS release dependencies without linking Homebrew libraries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
LOCK = Path(__file__).with_name('sources.json')


def digest(path):
    """Return a file's SHA-256 without loading archives into memory."""
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    """Build into a dedicated, non-relocatable prefix and record its provenance."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive.')
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('This dependency lock currently targets macOS arm64.')
    lock = json.loads(LOCK.read_text())
    work = ROOT / 'cmake-deps/macos'
    prefix = work / 'arm64'
    downloads = work / 'downloads'
    for directory in (prefix, downloads):
        directory.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for key in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH',
                'LDFLAGS', 'CPPFLAGS', 'CFLAGS', 'CXXFLAGS', 'CMAKE_PREFIX_PATH',
                'PKG_CONFIG_PATH', 'DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH'):
        env.pop(key, None)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    target = lock['deployment_target']
    env.update(CC='/usr/bin/clang', CXX='/usr/bin/clang++', SDKROOT=sdk,
               MACOSX_DEPLOYMENT_TARGET=target, CMAKE_PREFIX_PATH=str(prefix),
               PKG_CONFIG_LIBDIR=str(prefix / 'lib/pkgconfig'), PKG_CONFIG_PATH='')
    flags = f'-arch arm64 -mmacosx-version-min={target} -isysroot {sdk}'
    env.update(CFLAGS=flags, CXXFLAGS=flags, LDFLAGS=flags)
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
              f'-DCMAKE_INSTALL_PREFIX={prefix}', f'-DCMAKE_PREFIX_PATH={prefix}',
              f'-DCMAKE_OSX_SYSROOT={sdk}', f'-DCMAKE_OSX_DEPLOYMENT_TARGET={target}',
              '-DCMAKE_OSX_ARCHITECTURES=arm64',
              '-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local',
              '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF']

    def run(command, cwd=ROOT):
        print('+', ' '.join(map(str, command)), flush=True)
        subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True)

    def download(name):
        item = lock['sources'][name]
        path = downloads / item['url'].rsplit('/', 1)[-1]
        if not path.exists():
            temporary = path.with_suffix(path.suffix + '.part')
            run(['/usr/bin/curl', '-fL', '--retry', '3', '-o', temporary, item['url']])
            temporary.replace(path)
        if digest(path) != item['sha256']:
            raise RuntimeError(f'SHA-256 mismatch: {path}')
        return path

    def source(name):
        path = work / 'src' / name
        if not path.exists():
            archive = download(name)
            extracting = path.with_name(path.name + '.extracting')
            if extracting.exists():
                shutil.rmtree(extracting)
            extracting.mkdir(parents=True)
            run(['/usr/bin/tar', '-xf', archive, '-C', extracting, '--strip-components=1'])
            extracting.rename(path)
        return path

    def cmake_build(name, options):
        build = work / 'build' / name
        run(['cmake', '-S', source(name), '-B', build, *common, *options])
        run(['cmake', '--build', build, '--parallel', args.jobs])
        run(['cmake', '--install', build])

    # Invalidate completed-step markers when recipes, source lock or SDK change.
    compiler = subprocess.check_output(['/usr/bin/clang', '--version'], text=True).strip()
    recipe_input = Path(__file__).read_bytes() + LOCK.read_bytes() + sdk.encode() + compiler.encode()
    for patch in sorted(Path(__file__).with_name('patches').glob('*.patch')):
        recipe_input += patch.read_bytes()
    recipe = hashlib.sha256(recipe_input).hexdigest()
    marker = work / 'build/recipe.sha256'
    if marker.exists() and marker.read_text() != recipe:
        raise RuntimeError('Dependency recipe/SDK changed. Use a fresh cmake-deps/macos directory.')
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(recipe)
    (prefix / 'manifest.json').unlink(missing_ok=True)

    def step(name, action):
        stamp = marker.parent / (name + '.done')
        if not stamp.exists():
            action()
            stamp.touch()

    def openssl():
        src = source('openssl')
        run(['/usr/bin/perl', src / 'Configure', 'darwin64-arm64-cc',
             f'--prefix={prefix}', f'--openssldir={prefix}/ssl', 'shared', 'no-tests'], src)
        run(['/usr/bin/make', f'-j{args.jobs}'], src)
        run(['/usr/bin/make', 'install_sw'], src)

    step('openssl', openssl)
    step('opus', lambda: cmake_build('opus', ['-DOPUS_BUILD_TESTING=OFF', '-DOPUS_BUILD_PROGRAMS=OFF', '-DBUILD_SHARED_LIBS=OFF']))
    step('miniupnpc', lambda: cmake_build('miniupnpc', ['-DUPNPC_BUILD_TESTS=OFF', '-DUPNPC_BUILD_SAMPLE=OFF', '-DUPNPC_BUILD_SHARED=ON', '-DUPNPC_BUILD_STATIC=ON']))
    step('boost', lambda: cmake_build('boost', ['-DBUILD_SHARED_LIBS=OFF', '-DBOOST_INCLUDE_LIBRARIES=asio;filesystem;log;program_options;system;locale;crc;format;process;property_tree;algorithm;preprocessor;scope;uuid', '-DBOOST_LOCALE_ENABLE_ICU=OFF', '-DBOOST_LOCALE_ENABLE_ICONV=ON', '-DBUILD_TESTING=OFF']))

    def apply_patch(name, component):
        patch = download(name)
        stamp = source(component) / ('.applied-' + patch.name)
        if stamp.exists():
            if stamp.read_text() != digest(patch):
                raise RuntimeError(f'Patch changed: {patch}')
            return
        sections = re.split(r'(?=^diff --git )', patch.read_text(), flags=re.MULTILINE)
        production_patch = source(component) / '.production.patch'
        production_patch.write_text(''.join(section for section in sections if not section.startswith('diff --git a/tests/')))
        run(['/usr/bin/patch', '-f', '-p1', '-i', production_patch], source(component))
        production_patch.unlink()
        stamp.write_text(digest(patch))

    def qtbase():
        for name in sorted(lock['sources']):
            if name.endswith('.diff') and 'qtbase' in name:
                apply_patch(name, 'qtbase')
        options = ['-DBUILD_SHARED_LIBS=ON', '-DQT_BUILD_EXAMPLES=OFF', '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_BENCHMARKS=OFF', '-DFEATURE_framework=OFF']
        for feature in ('glib', 'dbus', 'icu', 'openssl', 'cups', 'sql', 'testlib', 'network', 'printsupport',
                        'system_png', 'system_jpeg', 'system_pcre2', 'system_freetype',
                        'system_harfbuzz', 'system_zlib', 'system_doubleconversion', 'system_libb2'):
            options.append(f'-DFEATURE_{feature}=OFF')
        cmake_build('qtbase', options)

    def qtsvg():
        for name in sorted(lock['sources']):
            if name.endswith('.diff') and 'qtsvg' in name:
                apply_patch(name, 'qtsvg')
        cmake_build('qtsvg', ['-DQT_BUILD_EXAMPLES=OFF', '-DQT_BUILD_TESTS=OFF'])

    step('qtbase', qtbase)
    step('qtsvg', qtsvg)
    # Use macOS's libcurl, not Homebrew's curl and its dependency closure.
    pc = prefix / 'lib/pkgconfig/libcurl.pc'
    curl_header = Path(sdk, 'usr/include/curl/curlver.h').read_text()
    curl_version = re.search(r'#define LIBCURL_VERSION \"([^\"]+)\"', curl_header)[1]
    pc.write_text(f'prefix={sdk}/usr\nName: libcurl\nDescription: macOS SDK libcurl\nVersion: {curl_version}\nLibs: -lcurl\nCflags: -I${{prefix}}/include\n')
    manifest = dict(schema=1, architecture='arm64', deployment_target=target,
                    prefix=str(prefix), sdk=sdk, recipe=recipe, sources=lock['sources'],
                    compiler=compiler,
                    symlinks={str(p.relative_to(prefix)): os.readlink(p) for p in sorted(prefix.rglob('*')) if p.is_symlink()},
                    files={str(p.relative_to(prefix)): digest(p) for p in sorted(prefix.rglob('*')) if p.is_file() and not p.is_symlink()})
    (prefix / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Dependencies ready: {prefix}', flush=True)


if __name__ == '__main__':
    main()
