#!/usr/bin/env python3
"""Assemble a versioned agent release; this command never publishes it."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import tomllib
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def validate_native(report, dmg, commit, ca_hash):
    if report.get('verification_version') != 1 or report.get('source_commit') != commit:
        raise ValueError('Native verification must refer to the same source commit')
    if report.get('dmg_sha256') != sha256(dmg):
        raise ValueError('Native image differs from its verification report')
    if report.get('ca_bundle_sha256') != ca_hash or report.get('external_dependencies') != []:
        raise ValueError('Native trust bundle or runtime dependencies failed verification')
    if report.get('agent_control_present') is not True or report.get('platform') not in ('macos-arm64', 'macos-universal'):
        raise ValueError('Native report does not describe a supported agent-enabled image')


def zip_tree(path, archive, prefix):
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as output:
        for item in sorted(path.rglob('*')):
            if item.is_symlink():
                raise ValueError('Release skill must not contain symlinks')
            if item.is_file() and '__pycache__' not in item.parts and item.suffix != '.pyc' and item.name != '.DS_Store':
                output.write(item, str(Path(prefix) / item.relative_to(path)))


def release_metadata(root):
    config = tomllib.loads((root / 'agent-release.toml').read_text())
    revision = config['fork_revision']
    if type(revision) is not int or revision < 1:
        raise ValueError('Fork revision must be a positive integer')
    upstream_commit = config['upstream_commit']
    if not isinstance(upstream_commit, str) or not re.fullmatch(r'[0-9a-f]{40}', upstream_commit):
        raise ValueError('Upstream commit must be a full lowercase Git SHA')
    def upstream_version(text):
        match = re.search(r'^set\(SoftFever_VERSION "([0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?)"\)$', text, re.MULTILINE)
        if not match:
            raise ValueError('Cannot read upstream version from version.inc')
        return match[1]
    version = upstream_version((root / 'version.inc').read_text())
    pinned = subprocess.check_output(['git', '-C', str(root), 'show', upstream_commit + ':version.inc'], text=True)
    if upstream_version(pinned) != version:
        raise ValueError('Pinned upstream version differs from version.inc; update agent-release.toml after merging upstream')
    subprocess.run(['git', '-C', str(root), 'merge-base', '--is-ancestor', upstream_commit, 'HEAD'], check=True)
    mcp_version = tomllib.loads((root / 'mcp/pyproject.toml').read_text())['project']['version']
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', mcp_version):
        raise ValueError('MCP version must be a stable three-component package version')
    fork_version = f'{version}-agent.{revision}'
    return {'fork_version': fork_version, 'release_tag': 'v' + fork_version,
            'upstream_version': version, 'upstream_commit': upstream_commit,
            'fork_revision': revision, 'mcp_version': mcp_version}


def assemble(output, dmg, native_report, require_clean=False):
    metadata = release_metadata(ROOT)
    version = metadata['fork_version']
    commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain', '--untracked-files=normal'], text=True).strip())
    if require_clean and dirty:
        raise ValueError('Release assembly requires a clean, committed source tree')
    report = json.loads(native_report.read_text())
    validate_native(report, dmg, commit, sha256(ROOT / 'resources/cert/slicer_base64.cer'))
    if output.exists() and any(output.iterdir()):
        raise ValueError('Output directory must be new or empty; refusing to mix releases')
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='orca-agent-release-') as tmp:
        stage = Path(tmp)
        subprocess.run(['uv', 'build', '--directory', str(ROOT / 'mcp'), '--out-dir', str(stage)], check=True)
        image = stage / f'OrcaSlicer-agent-{version}-{report["platform"]}.dmg'
        shutil.copy2(dmg, image)
        shutil.copy2(native_report, stage / 'native-verification.json')
        zip_tree(ROOT / 'skills/orca-mcp-control', stage / f'orca-mcp-control-{version}.zip', 'orca-mcp-control')
        installer = stage / 'installer'; (installer / 'scripts/agent').mkdir(parents=True)
        for name in ('install.sh', 'setup_mcp.py'):
            shutil.copy2(ROOT / 'scripts/agent' / name, installer / 'scripts/agent' / name)
        shutil.copytree(ROOT / 'skills/orca-mcp-control', installer / 'skills/orca-mcp-control',
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.DS_Store'))
        shutil.copy2(ROOT / 'LICENSE.txt', installer / 'LICENSE.txt')
        (installer / 'docs/agent').mkdir(parents=True)
        for guide in ('INSTALL.md', 'INSTALL.ru.md'):
            shutil.copy2(ROOT / 'docs/agent' / guide, installer / 'docs/agent' / guide)
        (installer / 'README.md').write_text(
            f'# OrcaSlicer Agent {version} installer\n\n'
            'Unofficial fork of OrcaSlicer for autonomous AI agent workflows. AGPL-3.0.\n\n'
            'Install uv: https://docs.astral.sh/uv/getting-started/installation/\n'
            'Then run from this extracted folder (the bundled wheel is selected automatically):\n\n'
            '```sh\n./scripts/agent/install.sh install\n'
            './scripts/agent/install.sh doctor\n```\n\n'
            'The installer installs MCP and prints generic stdio configuration; install the companion DMG separately.\n'
            'Optional: add --client codex and/or --client zcode to configure those clients and install the skill.\n'
            'For other clients, copy skills/orca-mcp-control or follow its instructions manually.\n'
            'English and Russian installation guides are included in docs/agent/.\n'
            'Keep the CA certificate outside the app before replacing it. This installer does not modify app trust.\n'
            'Enable native control explicitly with ORCA_AGENT_CONTROL=1 in the Orca process.\n'
            'MCP works with any stdio client; omit --client to print generic configuration.\n'
            'Updates/removal preserve recipe databases and print journals.\n\n'
            f'Full instructions: https://github.com/outsid3rx/OrcaSlicer/blob/v{version}/docs/agent/INSTALL.md\n'
            'Upstream: https://github.com/OrcaSlicer/OrcaSlicer\n')
        wheels = list(stage.glob('*.whl'))
        if len(wheels) != 1:
            raise ValueError('Expected exactly one built wheel')
        shutil.copy2(wheels[0], installer / wheels[0].name)
        with tarfile.open(stage / f'orca-agent-installer-{version}.tar.gz', 'w:gz') as tar:
            tar.add(installer, arcname=f'orca-agent-installer-{version}')
        shutil.rmtree(installer)
        # GitHub's automatic source snapshots contain the same committed tree.
        # An explicit archive keeps the corresponding native source with releases.
        if not dirty:
            subprocess.run(['git', '-C', str(ROOT), 'archive', '--format=tar.gz',
                            f'--prefix=OrcaSlicer-agent-{version}/',
                            f'--output={stage / ("OrcaSlicer-agent-source-" + version + ".tar.gz")}', commit], check=True)
        files = [{'name': p.name, 'sha256': sha256(p), 'size': p.stat().st_size}
                 for p in sorted(stage.iterdir()) if p.is_file()]
        manifest = {'manifest_version': 1, **metadata,
                    'source_commit': commit, 'source_tree_dirty': dirty, 'publishable': not dirty,
                    'native': report, 'compatibility': {'wire_protocol': 1, 'native_api_revision_tested': 3,
                    'recipe_contract': 1, 'required_native_capabilities': ['system.describe', 'scene.describe',
                    'settings.describe', 'settings.get', 'settings.patch', 'jobs.get']}, 'files': files}
        (stage / 'release-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        checksummed = sorted(p for p in stage.iterdir() if p.is_file())
        (stage / 'SHA256SUMS').write_text(''.join(f'{sha256(p)}  {p.name}\n' for p in checksummed))
        for item in stage.iterdir():
            shutil.copy2(item, output / item.name)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dmg', required=True, type=Path)
    parser.add_argument('--native-report', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--require-clean', action='store_true')
    args = parser.parse_args()
    try:
        result = assemble(args.output.resolve(), args.dmg.resolve(strict=True), args.native_report.resolve(strict=True), args.require_clean)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Release assembly failed: {error}\n')
    print(json.dumps({'release_tag': result['release_tag'], 'publishable': result['publishable'], 'output': str(args.output)}, indent=2))


if __name__ == '__main__':
    main()
