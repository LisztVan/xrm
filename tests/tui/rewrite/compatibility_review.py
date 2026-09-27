#!/usr/bin/env python3
"""V07/V09: production TUI colour fallbacks and first-connect unavailable state.

FTXUI's current terminal.cpp treats TERM=xterm as Palette256, not Palette16.
TERM=vt100 with colour hints cleared exercises its Palette16 fallback. This
runs inside a VT-capable tmux host: TERM=dumb checks XRM monochrome rendering,
not the impossible claim that a cursor-addressed TUI works on a hardcopy TTY.
"""
import argparse
import json
import re
import shlex
from pathlib import Path

from terminal_regression import Harness, PAGES


MODES = {
    'xterm': {'TERM': 'xterm', 'COLORTERM': '', 'TERM_PROGRAM': '', 'WT_SESSION': ''},
    'palette16': {'TERM': 'vt100', 'COLORTERM': '', 'TERM_PROGRAM': '', 'WT_SESSION': ''},
    'dumb': {'TERM': 'dumb', 'COLORTERM': '', 'TERM_PROGRAM': '', 'WT_SESSION': ''},
    'no-color': {'TERM': 'xterm-256color', 'COLORTERM': '', 'TERM_PROGRAM': '',
                 'WT_SESSION': '', 'NO_COLOR': '1'},
    'first-no-daemon': {'TERM': 'xterm-256color', 'COLORTERM': '', 'TERM_PROGRAM': '', 'WT_SESSION': ''},
}


def colour_codes(ansi):
    found = []
    for sequence in re.findall(r'\x1b\[([0-9;]*)m', ansi):
        codes = [int(x or 0) for x in sequence.split(';')]
        index = 0
        while index < len(codes):
            code = codes[index]
            if code in (38, 48) and index + 2 < len(codes):
                mode = codes[index + 1]
                length = 3 if mode == 5 else 5
                found.append(codes[index:index+length])
                index += length
                continue
            if 30 <= code <= 37 or 40 <= code <= 47 or 90 <= code <= 97 or 100 <= code <= 107:
                found.append([code])
            index += 1
    return found


class CompatibilityHarness(Harness):
    def start_mode(self, profile, mode):
        hints = MODES[mode]
        # The assignments are in the pane command itself. Setting the parent
        # process env alone is insufficient because tmux supplies its own TERM.
        argv = ['env', '-u', 'NO_COLOR'] + [f'{k}={v}' for k, v in hints.items()]
        argv += [str(self.build / 'xrm'), '--socket', str(self.sock), 'tui']
        if profile:
            argv += ['--profile', profile]
        self.tmux('new-session', '-d', '-s', 'xrm', '-x', '80', '-y', '24',
                  'exec ' + shlex.join(argv))
        self.wait(lambda: 'TopNav' in self.grid(), 'initial terminal')
        pid = int(self.tmux('display-message', '-p', '-t', 'xrm', '#{pane_pid}').strip())
        actual = dict(item.split('=', 1) for item in Path(f'/proc/{pid}/environ').read_text().split('\0') if '=' in item)
        for key, value in hints.items():
            assert actual.get(key) == value, (key, actual.get(key), value)
        if 'NO_COLOR' not in hints:
            assert 'NO_COLOR' not in actual
        self.record('tui_environment', {'pid': pid, 'mode': mode,
                    'command': shlex.join(argv), 'actual': {k: actual.get(k) for k in ['TERM', 'COLORTERM', 'TERM_PROGRAM', 'WT_SESSION', 'NO_COLOR']}})

    def run_mode(self, mode):
        offline = mode == 'first-no-daemon'
        profile = None
        if offline:
            # No TUI has connected yet; replace the socket with a never-created
            # path after stopping our bootstrap fixture daemon. No cached model.
            self.daemon.terminate()
            self.daemon.wait(timeout=5)
            self.sock = self.temp / 'never-started.sock'
            assert not self.sock.exists()
        else:
            profile = self.rpc('profile.create', name='Compatibility', preset='custom')['id']
        self.start_mode(profile, mode)
        rendered_codes = []
        unavailable = {
            'Overview': 'Actual target', 'Profiles': 'Profiles unavailable',
            'Subscriptions': 'Subscriptions unavailable', 'Nodes': 'Nodes unavailable',
            'Logs': 'Log stream unavailable', 'Config': 'Core inventory unavailable',
        }
        for page in PAGES:
            self.page(page)
            self.content()
            screen = self.capture(page.lower())
            if offline:
                assert 'xrmd unavailable' in screen, screen
                assert 'Run unknown' in screen, screen
                if page in unavailable:
                    assert unavailable[page] in screen, screen
                assert '0 Profiles' not in screen and '0 sources' not in screen and '0 nodes' not in screen, screen
                assert 'No registered cores' not in screen and 'No Profiles' not in screen, screen
                if page == 'Logs':
                    assert 'Cached' not in screen and '0 text lines' not in screen, screen
                if page == 'Overview':
                    assert re.search(r'Actual target\s+unavailable', screen), screen
            if not offline and page == 'Overview':
                assert 'stopped' in screen and 'Process alive' not in screen, screen
            rendered_codes.extend(colour_codes(self.grid(True)))
            self.key('F1')
            self.contains('Help')
            self.capture(page.lower() + '-help')
            rendered_codes.extend(colour_codes(self.grid(True)))
            self.key('Escape')
        self.page('Profiles')
        self.content()
        self.key('n')
        self.contains('Name')
        form = self.capture('empty-form')
        rendered_codes.extend(colour_codes(self.grid(True)))
        assert 'Enter value' in form, form
        self.key('F1')
        self.contains('Help')
        self.capture('empty-form-help')
        rendered_codes.extend(colour_codes(self.grid(True)))
        self.key('Escape', 'Escape')
        if mode in ('dumb', 'no-color'):
            assert not rendered_codes, rendered_codes
        elif mode == 'palette16':
            assert rendered_codes, '16-colour mode emitted no semantic colours'
            assert all(len(code) == 1 or code[1] == 5 and code[2] < 16 for code in rendered_codes), sorted({tuple(code) for code in rendered_codes})
        elif mode == 'xterm':
            assert not any(len(code) == 5 for code in rendered_codes), 'Unexpected truecolour in Palette256 mode'
            assert any(len(code) == 3 and code[1] == 5 and code[2] >= 16 for code in rendered_codes), rendered_codes
        self.record('verification', {'status': 'PASS', 'mode': mode, 'pages': PAGES,
                    'colour_codes': sorted({tuple(code) for code in rendered_codes}),
                    'scope': 'seven pages and contextual Help, empty Profile form and its Help; no submission',
                    'limits': 'tmux VT host; terminal cells/ANSI not physical terminal pixels or OS IME'})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('out/dev'))
    parser.add_argument('--output', type=Path, default=Path('docs/evidence/review-remediation/compatibility'))
    parser.add_argument('--mode', action='append', choices=MODES,
                        help='Run selected modes; default runs every mode')
    args = parser.parse_args()
    for mode in (args.mode or MODES):
        harness = CompatibilityHarness(args.build, args.output / mode)
        try:
            harness.run_mode(mode)
        except Exception as exc:
            harness.capture('failure-final')
            harness.record('failure', repr(exc))
            raise
        finally:
            harness.close()
    print('Compatibility and first-no-daemon terminal review passed:', args.output)


if __name__ == '__main__':
    main()
