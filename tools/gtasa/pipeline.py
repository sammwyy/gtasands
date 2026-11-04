"""resources.toml driven asset build: sections, rules, cache, multiprocessing, report."""
import glob as globmod
import hashlib
import json
import os
import shutil
import struct
import sys
import time
import tomllib
from fnmatch import fnmatchcase
from multiprocessing import Pool
from pathlib import Path

from . import audio, ds_mesh, ds_misc, ds_radar, ds_texture, img, intro, vehicles, world
from .dsfmt import name as fname, write_pack

HANDLERS = {
    'texture': (ds_texture.DEFAULTS, lambda d, p, o: (ds_texture.convert_txd(d, p, o), {})),
    'model': (ds_mesh.DEFAULTS, lambda d, p, o: ds_mesh.convert_dff(d, p)),
    'collision': (ds_misc.COL_DEFAULTS, lambda d, p, o: ds_misc.convert_col(d, p)),
    'anim': (ds_misc.ANIM_DEFAULTS, lambda d, p, o: ds_misc.convert_ifp(d, p)),
    'audio': (audio.DEFAULTS, None),
}
EXT_HANDLER = {'.txd': 'texture', '.dff': 'model', '.col': 'collision', '.ifp': 'anim'}

TOOLS_DIR = Path(__file__).resolve().parent
# Cache keys include the source of the modules each handler depends on, so editing
# e.g. ds_texture.py only rebuilds textures. Update when adding imports to a converter.
HANDLER_SRC = {
    'texture': ['rw', 'txd', 'ds_texture', 'dsfmt'],
    'model': ['rw', 'dff', 'ds_mesh', 'dsfmt'],
    'collision': ['col', 'ds_misc', 'ds_mesh', 'dsfmt'],
    'anim': ['ifp', 'ds_misc', 'dsfmt'],
    'sfx': ['audio', 'dsfmt'],
    'stream': ['audio', 'dsfmt'],
    'radar': ['rw', 'txd', 'ds_texture', 'ds_radar', 'dsfmt'],
}
CODE_HASH = {h: hashlib.sha1(b''.join((TOOLS_DIR / f'{m}.py').read_bytes() for m in mods)).hexdigest()[:12]
             for h, mods in HANDLER_SRC.items()}


def match(name, pats):
    if isinstance(pats, str):
        pats = [pats]
    n = name.lower()
    return any(fnmatchcase(n, p.lower()) for p in pats)


class Config:
    def __init__(self, path):
        self.path = Path(path)
        self.root = self.path.parent
        raw = tomllib.loads(self.path.read_text())
        self.g = raw.get('global', {})
        self.source = Path(self.g.get('source', ''))
        self.game = self.root / self.g.get('game_dir', 'game')
        self.out = self.root / self.g.get('out_dir', 'nitrofs')
        self.cache = self.root / self.g.get('cache_dir', 'build/assets_cache')
        self.report = self.root / 'build' / 'assets_report.json'
        self.manifest = self.root / 'build' / 'assets_manifest.json'
        self.defaults = {h: {**d, **raw.get('defaults', {}).get(h, {})} for h, (d, _) in HANDLERS.items()}
        self.sections = {k: v for k, v in raw.items() if isinstance(v, dict) and 'kind' in v}

    def ide_names(self, files):
        """Model + TXD file names ('x.dff', 'x.txd') declared in IDE files (relative to game/,
        globs allowed, case-insensitive: "data/maps/**/*.ide")."""
        key = tuple([files] if isinstance(files, str) else files)
        cache = self.__dict__.setdefault('_ide', {})
        if key not in cache:
            names = set()
            paths = [p for f in key for p in (sorted(self.game.glob(f, case_sensitive=False)) if any(c in f for c in '*?[')
                                              else [self.game / f])]
            for p in paths:
                for line in (p.read_text(errors='replace').splitlines() if p.is_file() else []):
                    parts = [x.strip() for x in line.split('#', 1)[0].split(',')]
                    if len(parts) >= 3 and parts[0].isdigit():
                        names |= {parts[1].lower() + '.dff', parts[2].lower() + '.txd'}
            cache[key] = names
        return cache[key]

    def params(self, sec, handler, entry):
        """defaults -> section table -> matching rules, then global multipliers.
        Returns (params, texture_overrides, removed)."""
        p = {**self.defaults[handler], **sec.get(handler, {})}
        overrides, removed = [], match(entry, sec.get('remove', []))
        if sec.get('keep') and not match(entry, sec['keep']):
            removed = True
        for r in sec.get('rule', []):
            if 'match' in r and not match(entry, r['match']):
                continue
            if 'match_ide' in r and os.path.basename(entry).lower() not in self.ide_names(r['match_ide']):
                continue
            if 'match_texture' in r:
                overrides += [(pat, r.get('texture', {})) for pat in ([r['match_texture']] if isinstance(r['match_texture'], str) else r['match_texture'])]
                continue
            p.update(r.get(handler, {}))
            if 'remove' in r:
                removed = bool(r['remove'])
        g = self.g
        if handler == 'texture':
            p['scale'] *= g.get('texture_scale', 1.0)
        elif handler == 'model':
            p['ratio'] = min(1.0, p['ratio'] * g.get('model_ratio', 1.0))
        elif handler == 'collision':
            p['face_ratio'] = min(1.0, p['face_ratio'] * g.get('model_ratio', 1.0))
        elif handler == 'audio':
            p['sample_rate'] = int(p['sample_rate'] * g.get('audio_rate_scale', 1.0))
        return p, overrides, removed


# ---------------------------------------------------------------------------- worker

def _cache_path(cache, section, key):
    return cache / section / key[:2] / key


def run_task(t):
    key, cache, section, entry, handler, src, offset, size, params, overrides, force = t
    cp = _cache_path(cache, section, key)
    if not force and cp.exists():
        stats = json.loads(cp.with_suffix('.json').read_text())
        return entry, cp.read_bytes(), stats, None
    try:
        with open(src, 'rb') as f:
            f.seek(offset)
            data = f.read(size)
        if handler == 'sfx':
            out, stats = audio.convert_bank(data, params)
        elif handler == 'stream':
            out, stats = audio.convert_track(data, offset, params)
        elif handler == 'radar':
            out, stats = ds_radar.convert_radar(data, params)
        else:
            out, stats = HANDLERS[handler][1](data, params, overrides)
    except Exception as e:  # noqa: BLE001 - one bad entry must not stop the build
        return entry, None, {}, f'{type(e).__name__}: {e}'
    cp.parent.mkdir(parents=True, exist_ok=True)
    cp.write_bytes(out)
    cp.with_suffix('.json').write_text(json.dumps(stats))
    return entry, out, stats, None


def _key(handler, *parts):
    return hashlib.sha1(json.dumps([CODE_HASH[handler], handler, *parts], sort_keys=True, default=str).encode()).hexdigest()


# ---------------------------------------------------------------------------- builder

class Builder:
    def __init__(self, cfg, jobs=0, force=False, limit=0):
        self.cfg = cfg
        self.jobs = jobs or cfg.g.get('jobs', 0) or os.cpu_count()
        self.force = force
        self.limit = limit
        self.outputs = []
        self.report = {}

    def build(self, only=()):
        cfg = self.cfg
        cfg.out.mkdir(parents=True, exist_ok=True)
        t0 = time.time()
        with Pool(self.jobs) as pool:
            self.pool = pool
            for name, sec in cfg.sections.items():
                if only and name not in only:
                    continue
                if not sec.get('enabled', True):
                    print(f'[{name}] disabled')
                    continue
                st = self.report[name] = {'kind': sec['kind'], 'in': 0, 'out': 0, 'files': 0, 'removed': 0,
                                          'by_type': {}, 'stats': {}, 'errors': []}
                getattr(self, 'do_' + sec['kind'])(name, sec, st)
                print(f'[{name}] {st["files"]} files, {_mb(st["in"])} -> {_mb(st["out"])}'
                      f'{"  removed " + str(st["removed"]) if st["removed"] else ""}'
                      f'{"  ERRORS " + str(len(st["errors"])) if st["errors"] else ""}')
        if not only:
            self._prune()
        prev = json.loads(cfg.report.read_text()) if only and cfg.report.exists() else {}
        prev.update(self.report)
        cfg.report.parent.mkdir(parents=True, exist_ok=True)
        cfg.report.write_text(json.dumps(prev, indent=1))
        (cfg.out / '.stamp').touch()
        print(f'done in {time.time() - t0:.1f}s')
        print_report(cfg, prev)

    # -- helpers
    def _emit(self, rel, data):
        p = self.cfg.out / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        self.outputs.append(str(Path(rel)))
        return len(data)

    def _account(self, st, entry, n_in, n_out, stats=None):
        ext = os.path.splitext(entry)[1].lower() or entry
        t = st['by_type'].setdefault(ext, [0, 0, 0])
        t[0] += 1; t[1] += n_in; t[2] += n_out
        st['in'] += n_in
        for k, v in (stats or {}).items():
            st['stats'][k] = st['stats'].get(k, 0) + v

    def _run(self, name, st, tasks):
        res = {}
        total, last = len(tasks), time.time()
        for i, (entry, out, stats, err) in enumerate(self.pool.imap_unordered(run_task, tasks, chunksize=4)):
            if err:
                st['errors'].append(f'{entry}: {err}')
            else:
                res[entry] = (out, stats)
            if time.time() - last > 2:
                last = time.time()
                print(f'  [{name}] {i + 1}/{total}', file=sys.stderr)
        for e in st['errors'][:10]:
            print(f'  ! {e}', file=sys.stderr)
        return res

    def _task(self, name, entry, handler, src, off, size, params, overrides=()):
        mt = os.stat(src).st_mtime_ns
        k = _key(handler, params, list(overrides), str(src), off, size, mt)
        return (k, self.cfg.cache, name, entry, handler, str(src), off, size, params, list(overrides), self.force)

    # -- section kinds
    def do_img(self, name, sec, st):
        src = self.cfg.game / sec['src']
        if not src.exists():
            st['errors'].append(f'missing {src} (run: tools/assets.py sync)')
            return
        toc = img.read_toc(src)[:self.limit or None]
        tasks, copies = [], {}
        for entry, off, size in toc:
            h = EXT_HANDLER.get(os.path.splitext(entry)[1].lower(), 'copy')
            p, ov, removed = self.cfg.params(sec, h if h != 'copy' else 'texture', entry)
            if removed:
                st['removed'] += 1
                continue
            if h == 'copy':
                copies[entry] = img.read_entry(src, off, size)
                self._account(st, entry, size, size)
            else:
                tasks.append(self._task(name, entry, h, src, off, size, p, ov))
        sizes = {t[3]: t[7] for t in tasks}
        res = self._run(name, st, tasks)
        files = dict(copies)
        for entry, (out, stats) in res.items():
            files[entry] = out
            self._account(st, entry, sizes[entry], len(out), stats)
        write_pack(self.cfg.out / sec['out'], files)
        self.outputs.append(sec['out'])
        st['out'] = (self.cfg.out / sec['out']).stat().st_size
        st['files'] = len(files)

    def _globs(self, base, pats):
        found = {}
        for pat in pats:
            for p in Path(base).glob(pat, case_sensitive=False):
                if p.is_file():
                    found[p.relative_to(base)] = p
        return found

    def do_files(self, name, sec, st):
        files = self._globs(self.cfg.game, sec['src'])
        tasks, rels = [], {}
        for rel, p in sorted(files.items())[:self.limit or None]:
            entry = str(rel)
            h = EXT_HANDLER.get(p.suffix.lower(), 'copy')
            pp, ov, removed = self.cfg.params(sec, h if h != 'copy' else 'texture', entry)
            if removed:
                st['removed'] += 1
                continue
            out_rel = Path(sec.get('out', '')) / (entry if sec.get('keep_paths', True) else rel.name).lower()
            if h == 'copy':
                n = self._emit(out_rel, p.read_bytes())
                st['out'] += n
                self._account(st, entry, n, n)
                st['files'] += 1
                continue
            rels[entry] = out_rel
            tasks.append(self._task(name, entry, h, p, 0, p.stat().st_size, pp, ov))
        sizes = {t[3]: t[7] for t in tasks}
        for entry, (out, stats) in self._run(name, st, tasks).items():
            st['out'] += self._emit(rels[entry], out)
            self._account(st, entry, sizes[entry], len(out), stats)
            st['files'] += 1

    def do_copy(self, name, sec, st):
        for rel, p in sorted(self._globs(self.cfg.game, sec['src']).items()):
            if match(rel.name, sec.get('remove', [])) or match(str(rel), sec.get('remove', [])):
                st['removed'] += 1
                continue
            n = self._emit(Path(sec.get('out', '')) / str(rel).lower(), p.read_bytes())
            self._account(st, rel.name, n, n)
            st['out'] += n
            st['files'] += 1

    def do_intro(self, name, sec, st):
        """movies/*.mpg -> DVID (paletted frames + interleaved PCM16)."""
        fps = int(sec.get('fps', 10))
        rate = int(sec.get('audio_rate', 22050))
        for rel, p in sorted(self._globs(self.cfg.game, sec['src']).items()):
            dst = Path(sec.get('out', '')) / (p.stem.lower() + '.dvid')
            outp = self.cfg.out / dst
            code = str(fps) + str(rate) + str(p.stat().st_size)
            key = hashlib.sha1(code.encode()).hexdigest()
            kf = outp.with_suffix('.key')
            if outp.exists() and kf.exists() and kf.read_text() == key and not self.force:
                st['files'] += 1
                st['out'] += outp.stat().st_size
                st['in'] += p.stat().st_size
                continue
            outp.parent.mkdir(parents=True, exist_ok=True)
            n = intro.convert(str(p), str(outp), fps, rate)
            kf.write_text(key)
            self.outputs.append(str(dst))
            self._account(st, rel.name, p.stat().st_size, outp.stat().st_size, {'frames': n})
            st['files'] += 1
            st['out'] += 0
            print(f'  {rel.name} -> {n} frames', file=sys.stderr)

    def do_sfx(self, name, sec, st):
        root = self.cfg.game / sec.get('src', 'audio')
        cfgdir = root / 'CONFIG'
        if not (cfgdir / 'BankLkup.dat').exists():
            st['errors'].append(f'missing {cfgdir} (run: tools/assets.py sync)')
            return
        paks, banks = audio.sfx_tables(cfgdir)
        tasks, where = [], {}
        for bi, (pak, off, size) in enumerate(banks[:self.limit or None]):
            pn = paks[pak]
            p, _, removed = self.cfg.params(sec, 'audio', pn)
            src = root / 'SFX' / pn
            if not src.exists():
                src = root / 'sfx' / pn
            if removed or not src.exists():
                st['removed'] += 1
                continue
            entry = f'{pn}/{bi}'
            where[entry] = (pak, bi)
            tasks.append(self._task(name, entry, 'sfx', src, off, size + audio.BANK_HEADER, p))
        res = self._run(name, st, tasks)
        per_pak = {}
        for entry, (out, stats) in res.items():
            pak, bi = where[entry]
            per_pak.setdefault(pak, []).append((bi, out))
            self._account(st, paks[pak], stats.get('bytes_in', 0), len(out), {'sounds': stats.get('sounds', 0)})
        lookup = [(0xFF, 0)] * len(banks)
        out_dir = Path(sec.get('out', 'audio/sfx'))
        for pak, items in sorted(per_pak.items()):
            items.sort()
            blob = bytearray(struct.pack('<4sHH', b'DSFX', 1, len(items)))
            off = len(blob) + 12 * len(items)
            for i, (bi, b) in enumerate(items):
                blob += struct.pack('<IIHH', off, len(b), bi, 0)
                off += len(b)
                lookup[bi] = (pak, i)
            for _, b in items:
                blob += b
            st['out'] += self._emit(out_dir / f'{paks[pak].lower()}.dsfx', bytes(blob))
            st['files'] += 1
        lk = bytearray(struct.pack('<4sHBBHH', b'DSBL', 1, len(paks), 0, len(banks), 0))
        lk += b''.join(fname(p, 16) for p in paks)
        lk += b''.join(struct.pack('<BBH', p, 0, i) for p, i in lookup)
        st['out'] += self._emit(out_dir / 'banks.dsbl', bytes(lk))

    def do_streams(self, name, sec, st):
        root = self.cfg.game / sec.get('src', 'audio')
        cfgdir = root / 'CONFIG'
        if not (cfgdir / 'TrakLkup.dat').exists():
            st['errors'].append(f'missing {cfgdir} (run: tools/assets.py sync)')
            return
        packs, tracks = audio.stream_tables(cfgdir)
        tasks, where, count = [], {}, {}
        for ti, (pk, off, size) in enumerate(tracks):
            pn = packs[pk] if pk < len(packs) else ''
            if not pn:
                continue
            p, _, removed = self.cfg.params(sec, 'audio', pn)
            src = root / 'STREAMS' / pn
            if not src.exists():
                src = root / 'streams' / pn
            mx = p.get('max_tracks', 0) or self.limit
            if removed or not src.exists() or (mx and count.get(pn, 0) >= mx):
                st['removed'] += 1
                continue
            count[pn] = count.get(pn, 0) + 1
            entry = f'{pn}/{ti}'
            where[entry] = (pn, ti)
            p = {k: v for k, v in p.items() if k != 'max_tracks'}
            tasks.append(self._task(name, entry, 'stream', src, off, size + audio.TRACK_HEADER, p))
        per = {}
        for entry, (out, stats) in self._run(name, st, tasks).items():
            pn, ti = where[entry]
            per.setdefault(pn, []).append((ti, out))
            self._account(st, pn, stats.get('bytes_in', 0), len(out))
        out_dir = Path(sec.get('out', 'audio/streams'))
        for pn, items in sorted(per.items()):
            items.sort()
            blob = bytearray(struct.pack('<4sHH', b'DSST', 1, len(items)))
            off = len(blob) + 12 * len(items)
            for ti, b in items:
                blob += struct.pack('<III', ti, off, len(b))
                off += len(b)
            for _, b in items:
                blob += b
            st['out'] += self._emit(out_dir / f'{pn.lower()}.dsst', bytes(blob))
            st['files'] += 1

    def do_radar(self, name, sec, st):
        """radarNN.txd (12x12 grid) -> one DSRT file: header, presence map, then
        fixed-stride RGB15 tiles (64x64) and mips (32x32). Fixed offsets: the
        runtime seeks straight to a tile, no TOC and no decoding."""
        src = self.cfg.game / sec['src']
        if not src.exists():
            st['errors'].append(f'missing {src} (run: tools/assets.py sync)')
            return
        grid = int(sec.get('grid', 12))
        n = grid * grid
        tasks = []
        for entry, off, size in img.read_toc(src):
            stem = os.path.splitext(entry.lower())[0]
            if entry.lower().endswith('.txd') and stem[:5] == 'radar' and stem[5:].isdigit() and int(stem[5:]) < n:
                tasks.append(self._task(name, entry, 'radar', src, off, size, {}))
        sizes = {t[3]: t[7] for t in tasks}
        full_b, mip_b = ds_radar.FULL * ds_radar.FULL * 2, ds_radar.MIP * ds_radar.MIP * 2
        tiles = [None] * n
        for entry, (out, stats) in self._run(name, st, tasks).items():
            tiles[int(os.path.splitext(entry.lower())[0][5:])] = out
            self._account(st, entry, sizes[entry], len(out), stats)
        present = bytes(1 if t else 0 for t in tiles)
        hdr_size = 24 + len(present) + (-len(present) % 4)
        full_off = hdr_size
        mip_off = full_off + n * full_b
        blob = bytearray(struct.pack('<4sHHHHHHII', b'DSRT', 1, n, grid, ds_radar.FULL, ds_radar.MIP, 0,
                                     full_off, mip_off))
        blob += present + b'\0' * (hdr_size - 24 - len(present))
        blob += b''.join(t[:full_b] if t else b'\0' * full_b for t in tiles)
        blob += b''.join(t[full_b:] if t else b'\0' * mip_b for t in tiles)
        st['out'] += self._emit(sec['out'], bytes(blob))
        st['files'] = sum(present)

    def do_world(self, name, sec, st):
        """gta.dat IDE/IPL (+ stream IPLs in gta3.img) -> one DWLD sector file.
        Not cached per entry: a full pass over the map data takes a few seconds."""
        try:
            blob, stats = world.build(self.cfg.game, sec)
        except FileNotFoundError as e:
            st['errors'].append(str(e))
            return
        st['stats'] = stats
        st['out'] += self._emit(sec['out'], blob)
        st['files'] = 1

    def do_vehicles(self, name, sec, st):
        """vehicles.ide + handling.cfg + carcols.dat + surface tables -> one DVEH file."""
        try:
            blob, stats = vehicles.build(self.cfg.game, sec)
        except FileNotFoundError as e:
            st['errors'].append(str(e))
            return
        st['stats'] = stats
        st['out'] += self._emit(sec['out'], blob)
        st['files'] = 1

    def _prune(self):
        cfg = self.cfg
        old = set(json.loads(cfg.manifest.read_text())) if cfg.manifest.exists() else set()
        new = set(self.outputs)
        for rel in old - new:
            p = cfg.out / rel
            if p.is_file():
                p.unlink()
                print(f'  pruned {rel}')
        cfg.manifest.parent.mkdir(parents=True, exist_ok=True)
        cfg.manifest.write_text(json.dumps(sorted(new), indent=0))


# ---------------------------------------------------------------------------- sync / report

def sync(cfg, dry=False):
    """Copies from the PC install into game/ only what enabled sections need."""
    src = cfg.source
    if not src.is_dir():
        sys.exit(f'source not found: {src} (set [global].source)')
    want = set()
    for name, sec in cfg.sections.items():
        if not sec.get('enabled', True):
            continue
        k = sec['kind']
        if k in ('img', 'radar'):
            want.add(Path(sec['src']))
        elif k == 'world':
            want.add(Path(sec.get('img', 'models/gta3.img')))
            want |= {p.relative_to(src) for p in src.glob('data/**/*', case_sensitive=False)
                     if p.is_file() and p.suffix.lower() in ('.dat', '.ide', '.ipl')}
        elif k == 'vehicles':
            want |= {p.relative_to(src) for p in src.glob('data/*', case_sensitive=False)
                     if p.is_file() and p.suffix.lower() in ('.dat', '.ide', '.cfg')}
        elif k in ('files', 'copy', 'intro'):
            rm = sec.get('remove', [])
            for pat in sec['src']:
                want |= {r for p in src.glob(pat, case_sensitive=False) if p.is_file()
                         for r in [p.relative_to(src)] if not (match(str(r), rm) or match(r.name, rm))}
        elif k in ('sfx', 'streams'):
            base = Path(sec.get('src', 'audio'))
            want |= {p.relative_to(src) for p in (src / base / 'CONFIG').glob('*.dat')}
            sub = 'SFX' if k == 'sfx' else 'STREAMS'
            d = next((p for p in (src / base).iterdir() if p.name.upper() == sub), None)
            for p in (d.iterdir() if d else []):
                _, _, removed = cfg.params(sec, 'audio', p.name)
                if not removed:
                    want.add(p.relative_to(src))
    total = 0
    for rel in sorted(want, key=str):
        s, d = src / rel, cfg.game / rel
        if not s.exists():
            print(f'  ? missing in source: {rel}')
            continue
        total += s.stat().st_size
        if d.exists() and d.stat().st_size == s.stat().st_size and d.stat().st_mtime >= s.stat().st_mtime:
            continue
        print(f'  copy {rel} ({_mb(s.stat().st_size)})')
        if not dry:
            d.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(s, d)
    print(f'sync: {len(want)} files, {_mb(total)} needed in {cfg.game}')


def _mb(n):
    return f'{n / 1048576:.1f}MB'


def print_report(cfg, rep=None):
    rep = rep if rep is not None else json.loads(cfg.report.read_text())
    print(f'\n{"section":<16}{"files":>7}{"in":>11}{"out":>11}{"ratio":>7}  stats')
    total = 0
    for name, st in rep.items():
        total += st['out']
        ratio = f'{st["out"] / st["in"]:.2f}' if st['in'] else '-'
        stats = ' '.join(f'{k}={v}' for k, v in st['stats'].items())
        print(f'{name:<16}{st["files"]:>7}{_mb(st["in"]):>11}{_mb(st["out"]):>11}{ratio:>7}  {stats}')
        for ext, (n, i, o) in sorted(st['by_type'].items(), key=lambda kv: -kv[1][2])[:6]:
            print(f'  {ext:<14}{n:>7}{_mb(i):>11}{_mb(o):>11}')
        for e in st['errors'][:3]:
            print(f'  ! {e}')
        if len(st['errors']) > 3:
            print(f'  ! ... {len(st["errors"]) - 3} more errors (build/assets_report.json)')
    budget = cfg.g.get('budget_mb', 448)
    flag = 'OK' if total / 1048576 <= budget else 'OVER BUDGET'
    print(f'{"TOTAL":<16}{"":>7}{"":>11}{_mb(total):>11}   budget {budget}MB -> {flag}')
