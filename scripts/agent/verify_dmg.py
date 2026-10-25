#!/usr/bin/env python3
"""Inspect a macOS image without launching it or importing user trust material."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MAGIC = {bytes.fromhex(x) for x in ('cffaedfe', 'feedfacf', 'cefaedfe', 'feedface', 'cafebabe', 'bebafeca', 'cafebabf', 'bfbafeca')}


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def inspect_app(app: Path):
    """Check release isolation and record signature status, not notarization."""
    cert = app / 'Contents/Resources/cert/slicer_base64.cer'
    expected = ROOT / 'resources/cert/slicer_base64.cer'
    if cert.is_symlink() or cert.read_bytes() != expected.read_bytes():
        raise ValueError('Release CA bundle differs from the repository trust bundle')
    binaries, external = [], []
    for path in app.rglob('*'):
        if path.is_symlink():
            if not path.resolve().is_relative_to(app.resolve()):
                raise ValueError('App contains an external symlink: ' + str(path.relative_to(app)))
            continue
        if not path.is_file():
            continue
        with path.open('rb') as stream:
            magic = stream.read(4)
        if magic not in MAGIC:
            continue
        binaries.append(str(path.relative_to(app)))
        dependencies = subprocess.check_output(['otool', '-L', str(path)], text=True)
        for line in dependencies.splitlines()[1:]:
            dep = line.strip().split(' (')[0]
            if dep.startswith('/') and not dep.startswith(('/usr/lib/', '/System/Library/')):
                external.append({'file': str(path.relative_to(app)), 'dependency': dep})
    if not binaries or external:
        raise ValueError('App lacks Mach-O files or contains external dependencies: ' + str(external))
    executable = app / 'Contents/MacOS/OrcaSlicer'
    architecture = subprocess.check_output(['lipo', '-archs', str(executable)], text=True).strip().split()
    if set(architecture) not in ({'arm64'}, {'arm64', 'x86_64'}):
        raise ValueError('Only arm64 or universal macOS images are supported by this release workflow')
    # This is a compile-presence check, not live API or physical-printer proof.
    strings = subprocess.run(['strings', str(executable)], capture_output=True, text=True, check=True).stdout
    if 'ORCA_AGENT_CONTROL' not in strings:
        raise ValueError('Native image does not contain the opt-in AgentControl module')
    signature = subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], capture_output=True, text=True)
    return {'platform': 'macos-universal' if len(architecture) == 2 else 'macos-arm64',
            'architectures': sorted(architecture), 'mach_o_count': len(binaries),
            'external_dependencies': external, 'ca_bundle_sha256': sha256(cert),
            'signature_valid': signature.returncode == 0, 'notarization_verified': False,
            'agent_control_present': True, 'runtime_tested': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dmg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-commit', required=True, help='Commit that produced the image; workflow checks run provenance')
    args = parser.parse_args()
    image = args.dmg.resolve(strict=True)
    subprocess.run(['hdiutil', 'verify', str(image)], check=True, stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix='orca-agent-image-') as tmp:
        attached = subprocess.check_output(['hdiutil', 'attach', '-readonly', '-nobrowse', '-plist',
                                           '-mountpoint', tmp, str(image)])
        devices = plistlib.loads(attached)['system-entities']
        device = next(row['dev-entry'] for row in devices if row.get('mount-point'))
        try:
            apps = list(Path(tmp).glob('*.app'))
            if len(apps) != 1 or apps[0].name != 'OrcaSlicer.app':
                raise ValueError('Expected exactly one OrcaSlicer.app in the image')
            report = inspect_app(apps[0])
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True, stdout=subprocess.DEVNULL)
    report.update(source_commit=args.source_commit, dmg_sha256=sha256(image), verification_version=1)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
