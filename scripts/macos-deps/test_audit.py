#!/usr/bin/env python3
"""Exercise compatibility rejection using real Mach-O fixtures and dyld paths."""
import plistlib
import subprocess
import tempfile
import unittest
from pathlib import Path
from audit import audit, inspect_loads, resolve_dependency, version


class AuditTests(unittest.TestCase):
    """Cover incompatible OS targets, architecture, and dependency escapes."""

    def test_versions(self):
        self.assertEqual(version('14.4'), version('14.4.0'))
        self.assertGreater(version('14.5'), version('14.4'))

    def test_parse_loads(self):
        text = '\nLoad command 0\n cmd LC_BUILD_VERSION\n minos 27.0\n sdk 27.2\nLoad command 1\n cmd LC_LOAD_WEAK_DYLIB\n name /usr/lib/libSystem.B.dylib (offset 24)\nLoad command 2\n cmd LC_RPATH\n path @executable_path/../Frameworks (offset 12)\n'
        self.assertEqual(inspect_loads(text), (['27.0'], ['/usr/lib/libSystem.B.dylib'], ['@executable_path/../Frameworks']))

    def test_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / 'Test.app'
            library = app / 'Contents/Frameworks/test.dylib'
            library.parent.mkdir(parents=True)
            library.touch()
            binary = app / 'Contents/MacOS/Test'
            self.assertTrue(resolve_dependency('@rpath/test.dylib', binary, app, ['@executable_path/../Frameworks']))
            self.assertFalse(resolve_dependency('/opt/homebrew/lib/test.dylib', binary, app, []))
            self.assertFalse(resolve_dependency('@rpath/missing.dylib', binary, app, ['@executable_path/../Frameworks']))
            escape = library.parent / 'escape.dylib'
            escape.symlink_to('/usr/lib/libSystem.B.dylib')
            self.assertFalse(resolve_dependency('@loader_path/../Frameworks/escape.dylib', binary, app, []))

    def test_real_binaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / 'Test.app'
            executable = app / 'Contents/MacOS/Test'
            executable.parent.mkdir(parents=True)
            with (app / 'Contents/Info.plist').open('wb') as stream:
                plistlib.dump({'CFBundleExecutable': 'Test'}, stream)
            source = Path(temporary) / 'main.c'
            source.write_text('int main(void) { return 0; }\n')
            def compile_target(target):
                subprocess.run(['/usr/bin/clang', '-arch', 'arm64', f'-mmacosx-version-min={target}', str(source), '-o', str(executable)], check=True)
            compile_target('14.4')
            self.assertEqual(audit(app, '14.4', 'arm64'), [])
            self.assertTrue(any('missing x86_64' in e for e in audit(app, '14.4', 'x86_64')))
            compile_target('15.0')
            self.assertTrue(any('minimum OS' in e for e in audit(app, '14.4', 'arm64')))

    def test_empty_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.assertEqual(audit(Path(temporary), '14.4', 'arm64'), ['No Mach-O binaries found.'])


if __name__ == '__main__':
    unittest.main()
