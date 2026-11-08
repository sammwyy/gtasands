#!/usr/bin/env python3
"""GTA:SA -> NDS asset pipeline. Configured by resources.toml at the repo root.

  tools/assets.py sync                    copy needed files from the PC install into game/
  tools/assets.py build [SECTION..]       convert game/ -> nitrofs/ (cached, parallel)
        -j N  --force  --limit N
  tools/assets.py report                  size breakdown of the last build vs budget
  tools/assets.py list SECTION [-n 30]    largest entries of a section + resolved params
  tools/assets.py info PATH[:ENTRY]       inspect img / txd / dff / col / ifp
  tools/assets.py preview PATH[:ENTRY] [TEXTURE] [--section S]
                                          write original vs DS PNGs to build/preview/
"""
import argparse
import os
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gtasa import col, dff, ds_texture, ifp, img, txd  # noqa: E402
from gtasa.pipeline import EXT_HANDLER, Builder, Config, print_report, sync  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def load(cfg, spec):
    """'models/gta3.img:infernus.txd' -> (entry_name, bytes). Paths are relative to game/."""
    path, _, entry = spec.partition(':')
    p = Path(path)
    if not p.is_absolute():
        p = cfg.game / p if (cfg.game / p).exists() else Path(path)
    if not entry:
        p = p.resolve()
        return (str(p.relative_to(cfg.game.resolve())) if p.is_relative_to(cfg.game.resolve()) else p.name), p.read_bytes()
    for n, off, size in img.read_toc(p):
        if n.lower() == entry.lower():
            return n, img.read_entry(p, off, size)
    sys.exit(f'{entry} not found in {p}')


def write_png(path, w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def cmd_info(cfg, a):
    name, data = load(cfg, a.spec)
    ext = os.path.splitext(name)[1].lower()
    if ext == '.img':
        toc = img.read_toc(cfg.game / a.spec if (cfg.game / a.spec).exists() else a.spec)
        by = {}
        for n, _, s in toc:
            e = os.path.splitext(n)[1].lower()
            c = by.setdefault(e, [0, 0])
            c[0] += 1; c[1] += s
        print(f'{name}: {len(toc)} entries')
        for e, (c, s) in sorted(by.items(), key=lambda kv: -kv[1][1]):
            print(f'  {e:<6}{c:>7}{s / 1048576:>10.1f}MB')
    elif ext == '.txd':
        for t in txd.parse(data):
            print(' ', t)
    elif ext == '.dff':
        m = dff.parse(data)
        print(f'{name}: {len(m.frames)} frames, {len(m.geometries)} geometries, {len(m.atomics)} atomics')
        for f in m.frames:
            print(f'  frame {f.name!r} parent={f.parent} bone={f.bone_id}')
        for i, g in enumerate(m.geometries):
            print(f'  geom {i}: {len(g.verts)} verts {len(g.tris)} tris uv={g.uvs is not None} '
                  f'prelit={g.colors is not None} normals={g.normals is not None} skin={g.skin is not None} '
                  f'materials={[mt.texture for mt in g.materials]}')
    elif ext == '.col':
        for c in col.parse(data):
            print(f'  COL{c.version} {c.name!r} id={c.model_id} spheres={len(c.spheres)} boxes={len(c.boxes)} '
                  f'verts={len(c.verts)} faces={len(c.faces)} r={c.radius:.1f}')
    elif ext == '.ifp':
        block, anims = ifp.parse(data)
        print(f'{name}: block {block!r}, {len(anims)} anims')
        for an in anims[:40]:
            print(f'  {an.name:<24} seqs={len(an.seqs)} keys={sum(len(s.frames) for s in an.seqs)}')
    else:
        print(f'{name}: {len(data)} bytes')


def cmd_preview(cfg, a):
    name, data = load(cfg, a.spec)
    sec = cfg.sections.get(a.section, {})
    p, ov, _ = cfg.params(sec, 'texture', name)
    out = ROOT / 'build' / 'preview'
    out.mkdir(parents=True, exist_ok=True)
    for t in txd.parse(data):
        if a.texture and t.name.lower() != a.texture.lower():
            continue
        tp = dict(p)
        for pat, op in ov:
            from fnmatch import fnmatchcase
            if fnmatchcase(t.name.lower(), pat.lower()):
                tp.update(op)
        w, h, px = txd.decode(t, 0)
        write_png(out / f'{t.name}_orig.png', w, h, px)
        b = ds_texture.convert_texture(t, tp)
        dw, dh, dpx = ds_texture.decode_dtex(b)
        write_png(out / f'{t.name}_ds.png', dw, dh, dpx)
        print(f'  {t.name}: {w}x{h} ({len(t.levels[0])}B) -> {dw}x{dh} fmt={b[8]} ({len(b)}B)')
    print(f'previews in {out}')


def cmd_list(cfg, a):
    sec = cfg.sections[a.section]
    if sec['kind'] != 'img':
        sys.exit('list only supports img sections')
    toc = img.read_toc(cfg.game / sec['src'])
    toc.sort(key=lambda e: -e[2])
    for n, _, s in toc[:a.n]:
        h = EXT_HANDLER.get(os.path.splitext(n)[1].lower(), 'copy')
        p, ov, removed = cfg.params(sec, h if h != 'copy' else 'texture', n)
        keys = {'texture': ('scale', 'max_size', 'format'), 'model': ('ratio', 'max_tris', 'normals'),
                'collision': ('mode', 'face_ratio'), 'anim': ('keep_every',)}.get(h, ())
        desc = 'REMOVED' if removed else ' '.join(f'{k}={p[k]}' for k in keys)
        print(f'  {n:<26}{s / 1024:>9.0f}KB  {h:<9} {desc}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--config', default=str(ROOT / 'resources.toml'))
    sp = ap.add_subparsers(dest='cmd', required=True)
    b = sp.add_parser('build')
    b.add_argument('sections', nargs='*')
    b.add_argument('-j', '--jobs', type=int, default=0)
    b.add_argument('--force', action='store_true')
    b.add_argument('--limit', type=int, default=0, help='only first N entries per section (quick tests)')
    s = sp.add_parser('sync')
    s.add_argument('--dry-run', action='store_true')
    s.add_argument('sections', nargs='*')
    sp.add_parser('report')
    i = sp.add_parser('info')
    i.add_argument('spec')
    pv = sp.add_parser('preview')
    pv.add_argument('spec')
    pv.add_argument('texture', nargs='?')
    pv.add_argument('--section', default='gta3_img')
    ls = sp.add_parser('list')
    ls.add_argument('section')
    ls.add_argument('-n', type=int, default=30)
    a = ap.parse_args()
    cfg = Config(a.config)
    if a.cmd in ('sync', 'build') and getattr(a, 'sections', []) == ['radio_streams']:
        cfg.out = ROOT / 'build' / 'radio' / 'gtasads'
        cfg.report = ROOT / 'build' / 'radio' / 'report.json'
        cfg.manifest = ROOT / 'build' / 'radio' / 'manifest.json'
    if a.cmd == 'build':
        unknown = [x for x in a.sections if x not in cfg.sections]
        if unknown:
            sys.exit(f'unknown sections: {unknown}; available: {list(cfg.sections)}')
        Builder(cfg, a.jobs, a.force, a.limit).build(a.sections)
    elif a.cmd == 'sync':
        sync(cfg, a.dry_run, a.sections)
    elif a.cmd == 'report':
        print_report(cfg)
    elif a.cmd == 'info':
        cmd_info(cfg, a)
    elif a.cmd == 'preview':
        cmd_preview(cfg, a)
    elif a.cmd == 'list':
        cmd_list(cfg, a)


if __name__ == '__main__':
    main()
