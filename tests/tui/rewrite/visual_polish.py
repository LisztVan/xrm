#!/usr/bin/env python3
"""Real XRM/tmux visual evidence, isolated fixture and current-screen captures."""
import argparse
import json
from pathlib import Path
import time
from terminal_regression import Harness, PAGES, MODULES

class VisualHarness(Harness):
    def module(self, name):
        self.page('Modules'); self.key('Enter'); self.contains('SecondaryNav')
        current = next((line.strip().split(' · ')[0] for line in self.grid().splitlines() if ' · Source' in line), '')
        assert current in MODULES, current
        for _ in range((MODULES.index(name) - MODULES.index(current)) % len(MODULES)):
            self.key('Tab')
        self.key('Enter'); self.contains(name + ' · Source')

    def resize(self, width, height):
        self.tmux('resize-window', '-t', 'xrm', '-x', str(width), '-y', str(height))
        time.sleep(.3)

    def run_visual(self):
        a, b = self.fixture()
        # Exercise real records through the daemon's owned process pipe.
        p = self.rpc('profile.get', profile_id=a, include_source_text=True)
        source = p.pop('source_text')
        p['environment']['process_environment'].update({
            'XRM_FAKE_CORE_STDOUT': '2026/09/26 12:41:02.031 [Info] transport/internet/tcp: accepted connection from 127.0.0.1:51002\n'
                '2026/09/26 12:41:03.110 [Warning] app/dns: resolver timeout; preserving the configured fallback order\n'
                '2026/09/26 12:41:04.820 [Error] proxy/vless: loopback fixture rejected a deliberately invalid request; this long message must remain aligned when wrapped at 80 columns\n'
                'native output without severity or timestamp\n',
            'XRM_FAKE_CORE_LOG_COUNT': '8', 'XRM_FAKE_CORE_LOG_INTERVAL_MS': '500'})
        self.rpc('profile.save', profile_id=a, settings=p, expected_text=source)
        self.operation('core.restart', profile_id=a)
        self.rpc('subscription.add', name='Local lab source', url='http://127.0.0.1:9/fixture', format='auto')
        self.key('Enter', 'r', 'Escape')
        for width, height in [(80, 24), (120, 40), (180, 48)]:
            self.resize(width, height)
            for page in PAGES:
                if page == 'Modules': self.module('dns')
                else: self.page(page); self.content()
                if page == 'Profiles' and width >= 120:
                    self.profile_cursor(b)
                    self.contains('Viewing: Alpha')
                    assert 'Beta' in self.grid()
                    self.capture(f'{width}x{height}-profile-other-cursor')
                    self.profile_cursor(a)
                    self.contains('Loopback'); self.contains('inventory')
                if page == 'Logs':
                    self.key('p', 'End')
                    time.sleep(.2)
                    self.key('p')
                time.sleep(.15)
                screen = self.capture(f'{width}x{height}-{page.lower()}')
                assert len(screen.splitlines()) == height
                assert 'F1 Help' in screen and page in screen
        self.resize(80, 24)
        self.page('Profiles'); self.content(); self.profile_cursor(a)
        self.key('e'); self.contains('Module Edit: Alpha')
        self.module('dns'); self.key('Enter', 'a'); self.contains('Address')
        self.type('9.9.9.9'); self.key('Tab', 'Space', 'Tab'); self.type('bad'); self.key('C-s')
        self.contains('Enter a whole number'); self.capture('form-validation-80x24')
        self.key('F1'); self.contains('Help'); self.capture('help-form-80x24'); self.key('Escape')
        self.key('Home','Delete','Delete','Delete'); self.type('53')
        self.key('Tab','Tab'); self.capture('form-string-list-80x24')
        self.key('C-s'); self.key('Escape'); self.key('F2'); self.contains('9.9.9.9')
        self.capture('module-json-80x24')
        before = self.runtime_files()
        self.key('C-s'); self.capture('toast-over-editor-80x24')
        assert before == self.runtime_files(), 'saving module changed runtime'
        time.sleep(3.3); self.capture('toast-expired-80x24')
        self.key('F2'); self.capture('module-visual-updated-80x24')
        self.page('Profiles'); self.content(); self.profile_cursor(a); self.key('R')
        self.key('End'); self.type(' polish')
        for w,h in [(120,40),(60,18),(180,48),(80,24)]:
            self.resize(w,h); self.capture(f'dirty-resize-{w}x{h}')
        self.key('Escape'); self.capture('dialog-unsaved-80x24')
        self.key('Tab','Enter')  # explicit discard
        self.page('Config'); self.content(); self.key('Escape')
        for name in ['Geodata', 'Tests', 'Appearance', 'Editor', 'General']:
            self.key('Tab', 'Enter'); self.capture('config-' + name.lower() + '-80x24')
            if name == 'Appearance':
                self.key('e'); self.capture('form-select-80x24'); self.key('Down','Space')
                self.capture('form-select-changed-80x24'); self.key('Escape','Tab','Enter')
            self.key('Escape')
        self.page('Profiles'); self.content(); self.profile_cursor(a); self.key('u')
        self.contains('Next: none'); self.capture('use-cancelled-80x24')
        assert self.rpc('runtime.status')['state'] == 'running'
        self.page('Logs'); self.content(); pid = self.rpc('runtime.status')['pid']
        self.key('Home'); self.capture('logs-paused-history-80x24')
        self.key('Enter'); self.capture('logs-record-detail-80x24'); self.key('Escape')
        assert self.rpc('runtime.status')['pid'] == pid
        # Same production process, palettes changed via its authoritative settings.
        for mode in ['256','none']:
            self.rpc('settings.update', patch={'appearance': {'color_mode': mode}})
            self.key('r'); time.sleep(.2); self.capture('logs-' + mode + '-80x24')
        self.rpc('settings.update', patch={'appearance': {'color_mode': 'truecolor', 'theme': 'light'}})
        self.key('r'); self.capture('logs-paper-80x24')
        self.rpc('settings.update', patch={'appearance': {'theme': 'dark'}})
        self.key('r')
        self.daemon.terminate(); self.daemon.wait(timeout=5)
        self.key('r'); self.capture('disconnected-80x24')
        self.record('verification', {'status': 'PASS', 'scope': 'all seven pages in 80x24/120x40/180x48; real PTY interaction, source-save boundary, log process stability, dirty resize, themes', 'core': 'fake process fixture, not connectivity evidence'})

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=Path('out/dev'))
    parser.add_argument('--output', type=Path, default=Path('docs/evidence/visual-polish'))
    args = parser.parse_args(); harness = VisualHarness(args.build, args.output)
    harness.env.pop("NO_COLOR", None)
    harness.env["COLORTERM"] = "truecolor"
    try: harness.run_visual()
    finally: harness.close()
