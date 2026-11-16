#!/usr/bin/env python3
"""Build a local SD package from an owned PC installation and a code-only ROM."""
import argparse
import json
import os
import sys
import tempfile
import zipfile
from pathlib import Path

from gtasa.pipeline import Builder, Config, sync
from gtasa.world import resolve

ROOT = Path(__file__).resolve().parent.parent


def package(source, rom, output, sections=(), jobs=0, config=ROOT / 'resources.toml',
            native_library=None):
    source, rom, output = Path(source).resolve(), Path(rom).resolve(), Path(output).resolve()
    if not source.is_dir():
        raise ValueError(f'game directory not found: {source}')
    if rom.suffix.lower() not in ('.nds', '.dsi') or not rom.is_file():
        raise ValueError('ROM must be an existing SD .nds or .dsi file')
    if rom.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('ROM is too large for a code-only SD build; do not package an embedded-assets ROM')
    if not output.parent.is_dir():
        raise ValueError(f'output directory not found: {output.parent}')
    if output.exists():
        raise ValueError(f'output already exists: {output}')
    if native_library:
        library = Path(native_library).resolve()
        if not library.is_file():
            raise ValueError(f'native DTEX library not found: {library}')
        os.environ['GTASADS_NATIVE_LIB'] = str(library)
    cfg = Config(config)
    unknown = set(sections) - cfg.sections.keys()
    if unknown:
        raise ValueError(f'unknown sections: {sorted(unknown)}')
    if sections and any(cfg.sections[s].get('sd_only') for s in sections):
        raise ValueError('SD-only radio is not supported in this package yet')
    if not sections:
        required = ('models/gta3.img', 'models/gta_int.img', 'anim/anim.img',
                    'data/gta.dat', 'text/american.gxt')
        missing = [name for name in required if resolve(source, name) is None]
        if missing:
            raise ValueError(f'game installation is missing required files: {", ".join(missing)}')
    with tempfile.TemporaryDirectory(prefix='gtasads-package-') as tmp:
        work = Path(tmp)
        cfg.source, cfg.game, cfg.out = source, work / 'game', work / 'gtasads'
        cfg.cache = work / 'cache'
        cfg.report, cfg.manifest = work / 'report.json', work / 'manifest.json'
        sync(cfg, only=sections, link=True)
        Builder(cfg, jobs=jobs).build(sections)
        errors = [f'{section}: {error}' for section, data in json.loads(cfg.report.read_text()).items()
                  for error in data['errors']]
        if errors:
            raise RuntimeError('asset conversion failed:\n' + '\n'.join(errors[:20]))
        if not any(p.is_file() for p in cfg.out.rglob('*')):
            raise RuntimeError('asset conversion produced no files')
        with tempfile.NamedTemporaryFile(prefix='.gtasads-', suffix='.zip',
                                         dir=output.parent, delete=False) as file:
            partial = Path(file.name)
        try:
            with zipfile.ZipFile(partial, 'w', allowZip64=True) as archive:
                archive.write(rom, f'gtasads{rom.suffix.lower()}', compress_type=zipfile.ZIP_STORED)
                for path in sorted(cfg.out.rglob('*')):
                    if path.is_file() and path.name != '.stamp' and path.suffix != '.key':
                        archive.write(path, f'gtasads/{path.relative_to(cfg.out).as_posix()}',
                                      compress_type=zipfile.ZIP_STORED)
            partial.replace(output)
        finally:
            partial.unlink(missing_ok=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='original GTA San Andreas installation directory')
    parser.add_argument('--rom', type=Path, required=True, help='code-only SD ROM (.nds or .dsi)')
    parser.add_argument('--output', type=Path, required=True, help='local ZIP to create')
    parser.add_argument('--section', action='append', default=[], help='build only this section (testing)')
    parser.add_argument('-j', '--jobs', type=int, default=0)
    parser.add_argument('--native-lib', type=Path, help='optional compiled native assets library')
    args = parser.parse_args()
    try:
        print(package(args.source, args.rom, args.output, args.section, args.jobs,
                      native_library=args.native_lib))
    except (ValueError, RuntimeError, OSError) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
