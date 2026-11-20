#!/usr/bin/env python3
"""Check that cached dependencies cannot silently change between releases."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from build import digest


class VerifyTests(unittest.TestCase):
    """Exercise checksum, unexpected-file, and symlink validation."""

    def test_integrity(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary).resolve()
            data = prefix / 'library.a'
            data.write_bytes(b'original')
            link = prefix / 'library-current.a'
            link.symlink_to('library.a')
            manifest = dict(prefix=str(prefix), deployment_target='14.4', architecture='arm64',
                            ffmpeg_recipe='fixture', files={'library.a': digest(data)},
                            symlinks={'library-current.a': 'library.a'})
            (prefix / 'manifest.json').write_text(json.dumps(manifest))
            def verify():
                return subprocess.run([sys.executable, str(Path(__file__).with_name('verify.py')), str(prefix)], capture_output=True).returncode
            self.assertEqual(verify(), 0)
            data.write_bytes(b'changed')
            self.assertNotEqual(verify(), 0)
            data.write_bytes(b'original')
            extra = prefix / 'unexpected.dylib'
            extra.touch()
            self.assertNotEqual(verify(), 0)
            extra.unlink()
            link.unlink()
            link.symlink_to('other.a')
            self.assertNotEqual(verify(), 0)


if __name__ == '__main__':
    unittest.main()
