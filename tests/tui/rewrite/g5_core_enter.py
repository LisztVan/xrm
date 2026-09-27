#!/usr/bin/env python3
"""G5 regression: Config / Core Content Enter opens the selected core details."""

import argparse
from pathlib import Path
import shutil

from terminal_regression import Harness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('out/dev'))
    parser.add_argument('--output', type=Path,
                        default=Path('docs/evidence/g5-rc/core-enter'))
    args = parser.parse_args()
    h = Harness(args.build, args.output)
    try:
        fake = h.build / 'tests/xrm_fake_core'
        core = h.rpc('core.register', path=str(fake), name='G5 isolated core')
        profile = h.rpc('profile.create', name='G5 Core Enter',
                        core_id=core['id'], preset='custom')['id']
        h.start_tui(profile)
        h.page('Config')
        h.content()
        h.contains('G5 isolated core')
        before = h.rpc('runtime.status')
        h.capture('before-enter-80x24')
        h.key('Enter')
        screen = h.capture('after-enter-80x24')
        h.record('enter_result', {'core_id': core['id'],
                                 'details_open': 'Details' in screen,
                                 'runtime_unchanged': h.rpc('runtime.status') == before})
        assert 'Details' in screen, 'Config / Core Content Enter did not open details'
        assert 'G5 isolated core' in screen
        assert 'reported_version: Xray 26.9.9' in screen
        assert 'path: ' in screen
        assert h.rpc('runtime.status') == before
        h.key('Escape')
        restored = h.capture('after-esc-80x24')
        assert 'Details' not in restored
        assert 'G5 isolated core' in restored
        assert 'Page content (Content)' in restored
        assert h.rpc('runtime.status') == before
    finally:
        h.close()
        shutil.rmtree(h.temp)
        h.record('fixture_removed', str(h.temp))


if __name__ == '__main__':
    main()
