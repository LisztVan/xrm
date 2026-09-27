#!/usr/bin/env python3
"""G5 regression: Enter on a highlighted Nodes row opens its read-only details."""

import argparse
from pathlib import Path
import shutil

from terminal_regression import Harness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('out/dev'))
    parser.add_argument('--output', type=Path,
                        default=Path('docs/evidence/g5-rc/nodes-enter'))
    args = parser.parse_args()
    h = Harness(args.build, args.output)
    try:
        profile = h.rpc('profile.create', name='G5 Nodes Enter', preset='custom')['id']
        node = h.rpc('node.add', name='G5 loopback inventory',
                     outbound={'protocol': 'freedom', 'settings': {}})
        h.start_tui(profile)
        h.page('Nodes')
        h.content()
        h.contains('G5 loopback inventory')
        before = h.rpc('runtime.status')
        h.capture('before-enter-80x24')
        h.key('Enter')
        screen = h.capture('after-enter-80x24')
        h.record('enter_result', {'node_id': node['id'], 'details_open':
                                 'Details' in screen, 'runtime_unchanged':
                                 h.rpc('runtime.status') == before})
        assert 'Details' in screen, 'Nodes Content Enter did not open details'
        assert 'G5 loopback inventory' in screen
        assert h.rpc('runtime.status') == before
        h.key('Escape')
        restored = h.capture('after-esc-80x24')
        assert 'Details' not in restored
        assert 'G5 loopback inventory' in restored
        assert 'Page content (Content)' in restored
        assert h.rpc('runtime.status') == before
    finally:
        h.close()
        shutil.rmtree(h.temp)
        h.record('fixture_removed', str(h.temp))


if __name__ == '__main__':
    main()
