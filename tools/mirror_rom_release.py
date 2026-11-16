#!/usr/bin/env python3
"""Mirror verified code-only release ROMs for same-origin GitHub Pages downloads."""
import argparse
import hashlib
import json
import os
import tempfile
import urllib.request
from pathlib import Path
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parent.parent
LATEST = 'https://api.github.com/repos/sammwyy/gtasands/releases/latest'
MAX_ROM_SIZE = 16 * 1024 * 1024


def release():
    request = urllib.request.Request(
        LATEST, headers={'Accept': 'application/vnd.github+json',
                         'User-Agent': 'gtasads-rom-mirror'})
    with urllib.request.urlopen(request, timeout=30) as source:
        return json.load(source)


def mirror(data, output, opener=urllib.request.urlopen):
    output = Path(output)
    if not output.parent.is_dir():
        raise FileNotFoundError(f'output directory parent not found: {output.parent}')
    mirrored = []
    for asset in data.get('assets', []):
        name, asset_id = asset.get('name'), asset.get('id')
        size, digest = asset.get('size'), asset.get('digest', '')
        url = asset.get('browser_download_url', '')
        parsed = urlsplit(url)
        if (name not in ('gtasads.nds', 'gtasads.dsi') or
                not isinstance(asset_id, int) or asset_id <= 0 or
                not isinstance(size, int) or not 0 < size <= MAX_ROM_SIZE or
                not isinstance(digest, str) or not digest.startswith('sha256:') or
                len(digest) != 71 or any(char not in '0123456789abcdef'
                                         for char in digest[7:]) or
                parsed.scheme != 'https' or parsed.hostname != 'github.com' or
                not parsed.path.startswith('/sammwyy/gtasands/releases/download/')):
            continue
        target = output / str(asset_id) / name
        expected = digest[7:]
        if target.exists():
            if target.stat().st_size != size or hashlib.sha256(target.read_bytes()).hexdigest() != expected:
                raise FileExistsError(f'existing ROM differs from release: {target}')
            mirrored.append(target)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(prefix='.gtasads-', dir=target.parent,
                                         delete=False) as temporary:
            temp_path = Path(temporary.name)
            try:
                checksum = hashlib.sha256()
                received = 0
                with opener(url) as source:
                    while chunk := source.read(65536):
                        received += len(chunk)
                        if received > MAX_ROM_SIZE:
                            raise ValueError('release ROM exceeds SD size limit')
                        checksum.update(chunk)
                        temporary.write(chunk)
                if received != size or checksum.hexdigest() != expected:
                    raise ValueError(f'release ROM hash or size mismatch: {name}')
                os.link(temp_path, target)
            finally:
                temp_path.unlink(missing_ok=True)
        mirrored.append(target)
    if not mirrored:
        raise ValueError('latest release has no signed-size code-only SD ROM assets')
    return mirrored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'web/roms')
    args = parser.parse_args()
    try:
        for path in mirror(release(), args.output_dir):
            print(path)
    except (OSError, ValueError) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
