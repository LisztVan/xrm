#!/usr/bin/env python3
"""R7: current production module screens, isolated daemon and tmux resources.

Uses the existing Harness without modifying it. No core is registered or run;
the fixture only saves native source files, all explicitly excluded. Captures
are terminal cells/ANSI and HTML rerenders, not terminal-emulator pixel images.
"""
import argparse
import json
from pathlib import Path

from terminal_regression import Harness, MODULES


VALUES = {
    'log': {'access': 'none', 'error': 'none', 'loglevel': 'warning'},
    'dns': {'servers': [{'address': '1.1.1.1', 'port': 53, 'queryStrategy': 'UseIP'}],
            'hosts': {'full:example.test': ['127.0.0.1']}, 'disableCache': False,
            'futureNative': {'preserved': [False, 0, '']}},
    'inbounds': [{'tag': 'local-socks', 'protocol': 'socks', 'listen': '127.0.0.1',
                  'port': 19091, 'settings': {'auth': 'noauth', 'udp': False}}],
    'outbounds': [{'tag': 'direct', 'protocol': 'freedom', 'settings': {}}],
    'routing': {'domainStrategy': 'AsIs',
                'rules': [{'domain': ['domain:example.test'], 'outboundTag': 'direct',
                           'futureNative': False}],
                'balancers': [{'tag': 'native-auto', 'selector': ['direct']}]},
    'observatory': {'subjectSelector': ['direct'], 'probeInterval': '30s',
                    'enableConcurrency': False},
    'api': {'tag': 'control', 'services': ['StatsService']},
    'metrics': {'listen': '127.0.0.1:19092'},
    'stats': {},
    'policy': {'levels': {'0': {'handshake': 0, 'statsUserUplink': False}},
               'system': {'statsOutboundDownlink': False}},
    'fakedns': [{'ipPool': '198.18.0.0/15', 'poolSize': 65535}],
    'geodata': {'cron': '0 0 * * *',
                'assets': [{'url': 'https://example.invalid/geoip.dat', 'file': 'geoip.dat'}]},
    'env': {'XRAY_LOCATION_ASSET': '/tmp/xrm-review-fixture-assets'},
    'version': {'min': '26.9.9', 'max': ''},
    'burstObservatory': {'subjectSelector': ['direct'],
                         'pingConfig': {'destination': 'https://example.invalid/probe',
                                        'interval': '1m', 'sampling': 0, 'timeout': '5s'}},
}
COLLECTIONS = {'dns', 'routing', 'inbounds', 'outbounds', 'policy', 'fakedns', 'env'}


class ModuleHarness(Harness):
    def module(self, name):
        # Leaving the top page resets SecondaryNav to its first registered key;
        # navigation does not depend on a clipped footer presentation string.
        self.page('Profiles')
        self.page('Modules')
        self.key('Enter')
        self.contains('SecondaryNav')
        for _ in range(MODULES.index(name)):
            self.key('Tab')
        self.key('Enter')
        self.contains(name)
        self.contains('Content')

    def resize(self, width, height):
        self.tmux('resize-window', '-t', 'xrm', '-x', str(width), '-y', str(height))
        self.wait(lambda: len(self.grid().splitlines()) == height,
                  f'resize to {width}x{height}')

    def open_object(self, name, editing):
        self.key('Enter')
        if name in COLLECTIONS:
            self.key('e' if editing else 'Enter')
            if name in {'inbounds', 'outbounds'}:
                self.key('Enter')
        if name != 'stats':
            self.contains('Add to draft' if editing else 'READ ONLY')

    def run_modules(self, help_only=False):
        assert set(MODULES) == set(VALUES), 'Fixture must match all registered module keys'
        profile = self.rpc('profile.create', name='Module review', preset='custom')['id']
        assert [item['key'] for item in self.rpc('module.list', profile_id=profile)] == MODULES
        for key in MODULES:
            previous = self.rpc('module.get', profile_id=profile, module=key)
            params = {'profile_id': profile, 'module': key,
                      'text': json.dumps({key: VALUES[key]}, indent=2) + '\n'}
            if previous.get('text') is None:
                params['expected_absent'] = True
            else:
                params['expected_text'] = previous['text']
            self.rpc('module.save', **params)
            self.rpc('module.include', profile_id=profile, module=key, included=False)
        baseline = {key: self.rpc('module.get', profile_id=profile, module=key)['text']
                    for key in MODULES}
        runtime = self.runtime_files()
        self.start_tui(profile)
        self.record('fixture', {'profile': profile, 'modules': MODULES,
                               'included': False, 'core': 'not registered or started'})
        for width, height in [(80, 24), (120, 40), (180, 48)]:
            self.resize(width, height)
            for key in ([] if help_only else MODULES):
                self.module(key)
                self.capture(f'{width}x{height}-{key}-visual-readonly')
                self.open_object(key, False)
                self.capture(f'{width}x{height}-{key}-object-readonly')
            self.page('Profiles')
            self.content()
            self.profile_cursor(profile)
            self.key('e')
            for key in (["dns", "routing"] if help_only else MODULES):
                self.module(key)
                self.open_object(key, True)
                if not help_only:
                    self.capture(f'{width}x{height}-{key}-object-edit')
                if key in {'dns', 'routing'}:
                    self.key('F1')
                    self.contains('Help')
                    self.capture(f'{width}x{height}-{key}-form-help')
                    self.key('Escape')
                self.key('Enter' if key == 'stats' else 'C-s')
                self.wait(lambda: 'Add to draft' not in self.grid(),
                          f'{key}: unchanged object accepted')
            self.page('Profiles')
            self.content()
            self.profile_cursor(profile)
            self.key('e')
            for key in MODULES:
                saved = self.rpc('module.get', profile_id=profile, module=key)
                assert saved['text'] == baseline[key], key
                assert not saved['included'], key
            assert self.runtime_files() == runtime
        self.record('verification', {
            'status': 'PASS', 'scope': ('DNS/routing Help at 80/120/180 columns; '
            'unchanged acceptance; source and runtime unchanged' if help_only else
            'all 15 modules at 80/120/180 columns; '
            'Visual, readonly object, editable object, unchanged acceptance; '
            'source and runtime unchanged'), 'core': 'not started',
            'limits': 'No OS IME or native terminal pixel capture; no network validation'})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('out/dev'))
    parser.add_argument('--output', type=Path,
                        default=Path('docs/evidence/review-remediation/modules'))
    parser.add_argument('--help-only', action='store_true',
                        help='Recapture only the six affected native Form Help screens')
    args = parser.parse_args()
    harness = ModuleHarness(args.build, args.output)
    try:
        harness.run_modules(args.help_only)
    finally:
        harness.close()
