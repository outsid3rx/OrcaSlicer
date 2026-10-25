"""Release trust/provenance boundaries; no image mounting or publication."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('package_release', Path(__file__).resolve().parents[1] / 'package_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def test_native_report_binds_image_commit_and_trust(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'app.dmg'; image.write_bytes(b'fixture')
            report = {'verification_version': 1, 'source_commit': 'a' * 40,
                      'dmg_sha256': release.sha256(image), 'ca_bundle_sha256': 'ca',
                      'external_dependencies': [], 'platform': 'macos-arm64', 'agent_control_present': True}
            release.validate_native(report, image, 'a' * 40, 'ca')
            for key, value in [('source_commit', 'b' * 40), ('dmg_sha256', 'wrong'),
                               ('ca_bundle_sha256', 'personal'), ('external_dependencies', ['/opt/homebrew/lib.dylib']),
                               ('agent_control_present', False), ('platform', 'untested')]:
                with self.subTest(key=key), self.assertRaises(ValueError):
                    release.validate_native({**report, key: value}, image, 'a' * 40, 'ca')
            image.write_bytes(b'modified')
            with self.assertRaises(ValueError):
                release.validate_native(report, image, 'a' * 40, 'ca')

    def test_skill_archive_has_discoverable_single_root_and_no_bytecode(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / 'skill'; folder.mkdir()
            (folder / 'SKILL.md').write_text('name: fixture')
            (folder / '.DS_Store').write_bytes(b'local metadata')
            cache = folder / '__pycache__'; cache.mkdir(); (cache / 'test.pyc').write_bytes(b'x')
            archive = Path(tmp) / 'skill.zip'
            release.zip_tree(folder, archive, 'orca-mcp-control')
            with zipfile.ZipFile(archive) as packed:
                self.assertEqual(packed.namelist(), ['orca-mcp-control/SKILL.md'])

    def test_skill_archive_rejects_external_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / 'skill'; folder.mkdir()
            secret = Path(tmp) / 'outside'; secret.write_text('private data')
            (folder / 'reference.md').symlink_to(secret)
            with self.assertRaises(ValueError):
                release.zip_tree(folder, Path(tmp) / 'skill.zip', 'orca-mcp-control')


if __name__ == '__main__':
    unittest.main()
