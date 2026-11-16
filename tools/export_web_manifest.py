#!/usr/bin/env python3
"""Export browser-readable conversion rules without local installation paths."""
import argparse
import json
from pathlib import Path

from gtasa.pipeline import Config


ROOT = Path(__file__).resolve().parent.parent
GLOBAL_RULES = ('texture_scale', 'model_ratio', 'audio_rate_scale')


def export(config):
    return {
        'version': 1,
        'global': {name: config.g.get(name, 1.0) for name in GLOBAL_RULES},
        'defaults': config.defaults,
        'sections': config.sections
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=ROOT / 'resources.toml')
    parser.add_argument('--output', type=Path, default=ROOT / 'web/resources.json')
    args = parser.parse_args()
    data = json.dumps(export(Config(args.config)), sort_keys=True,
                      ensure_ascii=False, indent=2) + '\n'
    args.output.write_text(data, encoding='utf-8')


if __name__ == '__main__':
    main()
