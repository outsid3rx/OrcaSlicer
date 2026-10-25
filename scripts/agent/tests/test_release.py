"""Release trust/provenance boundaries; no image mounting or publication."""
import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
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


class ForkVersionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.git('init', '-q')
        self.git('config', 'user.name', 'Release fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        (self.root / 'version.inc').write_text('set(SoftFever_VERSION "2.5.0-dev")\n')
        (self.root / 'mcp').mkdir()
        (self.root / 'mcp/pyproject.toml').write_text('[project]\nversion = "0.4.0"\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'Upstream fixture')
        self.upstream = self.git('rev-parse', 'HEAD').strip()
        self.config()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True)

    def config(self, revision='1', upstream=None):
        (self.root / 'agent-release.toml').write_text(
            f'fork_revision = {revision}\nupstream_commit = "{upstream or self.upstream}"\n')

    def test_fork_release_preserves_upstream_and_independent_mcp_versions(self):
        metadata = release.release_metadata(self.root)
        self.assertEqual(metadata['fork_version'], '2.5.0-dev-agent.1')
        self.assertEqual(metadata['release_tag'], 'v2.5.0-dev-agent.1')
        self.assertEqual(metadata['mcp_version'], '0.4.0')
        self.assertEqual(metadata['upstream_commit'], self.upstream)
        self.config('2')
        self.assertEqual(release.release_metadata(self.root)['fork_version'], '2.5.0-dev-agent.2')
        (self.root / 'version.inc').write_text('set(SoftFever_VERSION "2.5.1")\n')
        self.git('add', 'version.inc'); self.git('commit', '-qm', 'Next upstream')
        self.config(upstream=self.git('rev-parse', 'HEAD').strip())
        self.assertEqual(release.release_metadata(self.root)['fork_version'], '2.5.1-agent.1')

    def test_invalid_fork_revision_and_abbreviated_pin_are_rejected(self):
        for revision in ('0', '-1', 'true', '"1"'):
            with self.subTest(revision=revision):
                self.config(revision)
                with self.assertRaises(ValueError):
                    release.release_metadata(self.root)
        self.config(upstream=self.upstream[:9])
        with self.assertRaises(ValueError):
            release.release_metadata(self.root)

    def test_upstream_version_change_requires_reviewed_pin(self):
        (self.root / 'version.inc').write_text('set(SoftFever_VERSION "2.5.1")\n')
        with self.assertRaisesRegex(ValueError, 'Pinned upstream version differs'):
            release.release_metadata(self.root)

    def test_pin_outside_release_ancestry_is_rejected(self):
        self.git('checkout', '--orphan', 'unrelated')
        self.git('commit', '-qm', 'Unrelated fixture')
        unrelated = self.git('rev-parse', 'HEAD').strip()
        self.git('checkout', '--detach', self.upstream)
        self.config(upstream=unrelated)
        with self.assertRaises(subprocess.CalledProcessError):
            release.release_metadata(self.root)


if __name__ == '__main__':
    unittest.main()
