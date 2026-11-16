import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def build_library(output):
    sources = sorted(path for path in (ROOT / 'tools/portable').glob('*.c')
                     if not path.stem.endswith('_cli'))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-shared', '-fPIC', *(str(path) for path in sources),
                    '-lm', '-o', str(output)], check=True)
