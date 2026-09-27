#!/usr/bin/env python3
"""Isolated real-tmux UI regression. Uses current pane cells and RPC facts.

No installed xrm, real subscriptions, system networking or production files.
The fake core is only a process/log fixture; it does not prove Xray networking.
"""
import argparse
import base64
import hashlib
import html
import http.server
import json
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import tempfile
import threading
import time
import traceback

PAGES = ['Overview', 'Profiles', 'Modules', 'Subscriptions', 'Nodes', 'Logs', 'Config']
MODULES = ['log', 'dns', 'inbounds', 'outbounds', 'routing', 'observatory', 'api', 'metrics', 'stats', 'policy', 'fakedns', 'geodata', 'env', 'version', 'burstObservatory']


def ansi_html(value):
    palette = ['#111827', '#ef4444', '#22c55e', '#eab308', '#60a5fa', '#c084fc', '#22d3ee', '#e5e7eb']
    state = {'fg': '#e5e7eb', 'bg': '#111827', 'bold': False, 'dim': False, 'reverse': False}
    result = ''
    for segment in re.split(r'(\x1b\[[0-9;]*m)', value):
        if segment.startswith('\x1b['):
            codes = [int(n or 0) for n in segment[2:-1].split(';')]
            index = 0
            while index < len(codes):
                code = codes[index]
                if code == 0: state.update(fg='#e5e7eb', bg='#111827', bold=False, dim=False, reverse=False)
                elif code == 1: state['bold'] = True
                elif code == 2: state['dim'] = True
                elif code == 7: state['reverse'] = True
                elif code == 22: state.update(bold=False, dim=False)
                elif code == 27: state['reverse'] = False
                elif 30 <= code <= 37: state['fg'] = palette[code - 30]
                elif 40 <= code <= 47: state['bg'] = palette[code - 40]
                elif 90 <= code <= 97: state['fg'] = palette[code - 90]
                elif 100 <= code <= 107: state['bg'] = palette[code - 100]
                elif code == 39: state['fg'] = '#e5e7eb'
                elif code == 49: state['bg'] = '#111827'
                elif code in [38, 48] and index + 2 < len(codes) and codes[index + 1] == 5:
                    n = codes[index + 2]
                    levels = [0, 95, 135, 175, 215, 255]
                    if n < 16: shade = palette[n % 8]
                    elif n < 232:
                        n -= 16
                        shade = '#%02x%02x%02x' % (levels[n // 36], levels[(n // 6) % 6], levels[n % 6])
                    else: shade = '#%02x%02x%02x' % ((8 + (n - 232) * 10,) * 3)
                    state['fg' if code == 38 else 'bg'] = shade
                    index += 2
                elif code in [38, 48] and index + 4 < len(codes) and codes[index + 1] == 2:
                    state['fg' if code == 38 else 'bg'] = '#%02x%02x%02x' % tuple(codes[index + 2:index + 5])
                    index += 4
                index += 1
            continue
        fg, bg = (state['bg'], state['fg']) if state['reverse'] else (state['fg'], state['bg'])
        # ANSI faint dims foreground, not the whole cell/background. The exact
        # faint intensity is terminal-dependent; this renderer uses 65% ink.
        ink = f'color-mix(in srgb, {fg} 65%, {bg})' if state['dim'] else fg
        style = f'color:{ink};background:{bg};font-weight:{"bold" if state["bold"] else "normal"}'
        result += '<span style="' + style + '">' + html.escape(segment) + '</span>'
    return result


class Harness:
    def __init__(self, build, output):
        self.build = build.resolve()
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.temp = Path(tempfile.mkdtemp(prefix='xrm-rewrite-ui-'))
        self.sock = self.temp / 'xrmd.sock'
        self.tmux_socket = self.temp / 'tmux.sock'
        self.events = []
        self.env = os.environ.copy()
        self.env.update(HOME=str(self.temp / 'home'), XDG_CONFIG_HOME=str(self.temp / 'config'),
                        XDG_DATA_HOME=str(self.temp / 'data'), XDG_CACHE_HOME=str(self.temp / 'cache'),
                        XDG_RUNTIME_DIR=str(self.temp / 'runtime'))
        for key in ['HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR']:
            Path(self.env[key]).mkdir(mode=0o700)
        self.server_log = (self.output / 'xrmd.log').open('w')
        self.daemon = subprocess.Popen([str(self.build / 'xrmd'), '--foreground', '--socket', str(self.sock),
                                        '--database', str(self.temp / 'state.sqlite')], env=self.env,
                                       stdout=self.server_log, stderr=subprocess.STDOUT)
        self.http = None
        self.wait(lambda: self.sock.exists(), 'daemon socket')
        self.record('baseline', {'build': str(self.build), 'root': str(self.temp), 'pid': self.daemon.pid,
                                 'xrm_sha256': hashlib.sha256((self.build / 'xrm').read_bytes()).hexdigest()})

    def record(self, action, value):
        self.events.append({'time': round(time.monotonic(), 3), 'action': action, 'value': value})
        (self.output / 'trace.json').write_text(json.dumps(self.events, ensure_ascii=False, indent=2))

    def wait(self, condition, description, timeout=6):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            result = condition()
            if result:
                return result
            time.sleep(.04)
        self.capture('failure') if self.tmux_socket.exists() else None
        raise AssertionError('Timed out: ' + description)

    def rpc(self, method, **params):
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(6)
            connection.connect(str(self.sock))
            connection.sendall((json.dumps({'v': 1, 'id': 1, 'method': method, 'params': params}) + '\n').encode())
            line = connection.makefile('rb').readline()
        result = json.loads(line)
        if not result['ok']:
            raise AssertionError(f'{method}: {result.get("error")}')
        return result['result']

    def operation(self, method, **params):
        response = self.rpc(method, **params)
        if 'operation_id' not in response:
            return response
        result = self.wait(lambda: self.finished(response['operation_id']), method, timeout=12)
        assert result['status'] == 'succeeded', result
        return result.get('result')

    def finished(self, op):
        result = self.rpc('operation.get', operation_id=op)
        return result if result['status'] not in ['pending', 'running'] else None

    def tmux(self, *args, check=True):
        return subprocess.run(['tmux', '-S', str(self.tmux_socket), '-f', '/dev/null', *args],
                              env=self.env, check=check, text=True, capture_output=True).stdout

    def start_tui(self, profile):
        command = shlex.join([str(self.build / 'xrm'), '--socket', str(self.sock), 'tui', '--profile', profile])
        self.tmux('new-session', '-d', '-s', 'xrm', '-x', '80', '-y', '24', 'exec ' + command)
        self.wait(lambda: 'TopNav' in self.grid(), 'initial terminal')
        self.record('tui_pid', self.tmux('display-message', '-p', '-t', 'xrm', '#{pane_pid}').strip())

    def grid(self, styled=False):
        return self.tmux('capture-pane', '-p', '-N', *(['-e'] if styled else []), '-t', 'xrm')

    def contains(self, text, timeout=6):
        return self.wait(lambda: text in self.grid(), 'current screen contains ' + repr(text), timeout)

    def key(self, *keys):
        for key in keys:
            self.record('key', key)
            self.tmux('send-keys', '-t', 'xrm', key)
            # Give the terminal parser its lone-Escape boundary; these are real
            # events, not direct calls to component handlers.
            time.sleep(.13 if key == 'Escape' else .045)

    def type(self, value):
        self.record('text', value)
        self.tmux('send-keys', '-t', 'xrm', '-l', value)
        time.sleep(.06)

    def capture(self, name):
        plain, ansi = self.grid(), self.grid(True)
        (self.output / (name + '.txt')).write_text(plain)
        (self.output / (name + '.ansi')).write_text(ansi)
        # An HTML rendering of real current cells, not a design mockup. The raw
        # ANSI capture remains the authority for terminal styles.
        document = '<!doctype html><meta charset="utf-8"><title>Actual tmux cells</title>'
        document += '<style>@font-face{font-family:Grid;src:url(file:///usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf)}@font-face{font-family:Grid;src:url(file:///usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf);font-weight:bold}span{box-decoration-break:clone;-webkit-box-decoration-break:clone;padding-block:2px 1px}body{margin:0;background:#131a23;color:#dce3eb;width:max-content}pre{font:14px/19px Grid,monospace;margin:12px;white-space:pre}</style>'
        document += '<pre>' + ansi_html(ansi) + '</pre>'
        (self.output / (name + '.html')).write_text(document)
        self.record('capture', name)
        return plain

    def page(self, name):
        for _ in range(8):
            if any('TopNav' in line and 'F1 Help' in line for line in self.grid().splitlines()):
                break
            self.key('Escape')
        self.contains('TopNav')
        screen = self.grid()
        current = next(i for i, page in enumerate(PAGES) if '[' + page + ']' in screen)
        for _ in range((PAGES.index(name) - current) % len(PAGES)):
            self.key('Tab')
        self.contains('[' + name + ']')

    def content(self):
        for _ in range(2):
            if any('Content' in line and 'F1 Help' in line for line in self.grid().splitlines()):
                return
            self.key('Enter')
        self.contains('Content')

    def module(self, name):
        self.page('Modules')
        self.key('Enter')
        self.contains('SecondaryNav')
        current = next((line.strip().split(' · ')[0] for line in self.grid().splitlines() if ' · Source' in line), '')
        assert current in MODULES, current
        for _ in range((MODULES.index(name) - MODULES.index(current)) % len(MODULES)):
            self.key('Tab')
        self.key('Enter')
        self.contains(name + ' · Source')

    def profile_cursor(self, profile):
        rows = self.rpc('profile.list')
        position = next(i for i, row in enumerate(rows) if row['id'] == profile)
        self.key('Home')
        for _ in range(position):
            self.key('Down')

    def source(self, profile, module):
        value = self.rpc('module.get', profile_id=profile, module=module)
        return json.loads(value['text']) if value.get('text') else None

    def runtime_files(self):
        return {str(p.relative_to(self.temp)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in self.temp.rglob('*.json') if '/runs/' in str(p) or '/runtime/' in str(p)}

    def close(self):
        if self.tmux_socket.exists():
            self.tmux('kill-server', check=False)
        if self.daemon.poll() is None:
            self.daemon.terminate()
            try:
                self.daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
                self.daemon.wait()
        if self.http:
            self.http.shutdown()
            self.http.server_close()
        self.server_log.close()
        self.record('cleanup', {'daemon_returncode': self.daemon.returncode,
                                'isolated_fixture_retained': str(self.temp)})

    def fixture(self):
        fake = self.build / 'tests/xrm_fake_core'
        assert fake.is_file(), fake
        core = self.rpc('core.register', path=str(fake), name='Process fixture')
        a = self.rpc('profile.create', name='Alpha', core_id=core['id'], preset='custom')
        b = self.rpc('profile.create', name='Beta', core_id=core['id'], preset='custom')
        a, b = a['id'], b['id']
        node = self.rpc('node.add', name='Loopback inventory', outbound={'protocol': 'freedom', 'settings': {}})
        self.rpc('connection.select', profile_id=a, target={'kind': 'node', 'id_or_tag': node['id']})
        for key, value in {
            'dns': {'servers': ['1.1.1.1'], 'futureNative': {'preserved': True}},
            'routing': {'rules': [{'type': 'field', 'ruleTag': 'Initial rule', 'domain': ['example.test'], 'outboundTag': 'direct', 'futureNative': 42}]},
            'outbounds': [{'protocol': 'freedom', 'tag': 'direct', 'settings': {}}],
        }.items():
            current = self.rpc('module.get', profile_id=a, module=key)
            params = {'profile_id': a, 'module': key, 'text': json.dumps({key: value}, indent=2) + '\n'}
            params.update(expected_text=current['text']) if current.get('text') is not None else params.update(expected_absent=True)
            self.rpc('module.save', **params)
            self.rpc('module.include', profile_id=a, module=key, included=True)
        p = self.rpc('profile.get', profile_id=a, include_source_text=True)
        source = p.pop('source_text')
        p.setdefault('environment', {})['process_environment'] = {
            'XRM_FAKE_CORE_STDOUT': '[Info] UI process fixture\ncontinued line\n',
            'XRM_FAKE_CORE_LOG_COUNT': '240', 'XRM_FAKE_CORE_LOG_INTERVAL_MS': '100'}
        self.rpc('profile.save', profile_id=a, settings=p, expected_text=source)
        self.operation('connection.connect', profile_id=a)
        assert self.rpc('runtime.status')['state'] == 'running'
        self.record('fixture', {'profile_a': a, 'profile_b': b, 'node': node['id'], 'core': core['id'],
                                'verification_scope': 'fake core for process/log behavior, not real Xray traffic'})
        self.runtime_baseline = self.runtime_files()
        self.start_tui(a)
        return a, b

    def run(self):
        a, b = self.fixture()
        self.profile_a = a
        self.capture('01-overview-80x24')
        self.page('Profiles'); self.content()
        self.profile_cursor(a); self.key('Enter')
        screen = self.capture('02-profiles-details-bottom')
        assert 'View Alpha · Core Process fixture' in '\n'.join(screen.splitlines()[-4:]), screen
        assert 'Plan:' not in '\n'.join(screen.splitlines()[2:-4])
        selected_before = self.rpc('profile.get', profile_id=a)['selected']
        self.key('u'); self.contains('Next: none')
        assert self.rpc('profile.get', profile_id=a)['selected'] == selected_before
        assert self.rpc('runtime.status')['state'] == 'running'
        self.capture('03-use-cancelled-core-running')
        self.key('e'); self.contains('Module Edit: Alpha')
        self.key('u'); self.contains('Next: Alpha')
        self.module('dns'); self.contains('Source editable')
        self.capture('04-dns-visual-sections')
        self.key('Enter'); self.contains('DNS servers')
        self.key('a'); self.contains('Address')
        self.type('9.9.9.9'); self.key('Tab', 'Space', 'Tab'); self.type('oops'); self.key('C-s')
        self.contains('Enter a whole number')
        self.capture('05-dns-invalid-form-keeps-values')
        self.key('Home', 'Delete', 'Delete', 'Delete', 'Delete'); self.type('53')
        self.key('BTab', 'BTab', 'End', 'Left'); self.type('0'); self.key('BSpace')
        self.key('F1'); self.contains('Help'); self.capture('06-help-over-form')
        self.key('Escape'); self.key('C-s')
        self.wait(lambda: 'DRAFT COLLECTION' in self.grid() and 'Add to draft' not in self.grid(), 'DNS form accepted')
        assert self.source(a, 'dns')['dns']['servers'] == ['1.1.1.1'], 'Child acceptance must only change draft'
        self.key('Escape', 'F2')
        self.contains('JSON'); self.contains('9.9.9.9')
        self.capture('07-dns-json-shared-draft')
        self.key('F2', 'C-s')
        self.wait(lambda: len(self.source(a, 'dns')['dns']['servers']) == 2, 'DNS source saved')
        assert self.source(a, 'dns')['dns']['futureNative'] == {'preserved': True}
        assert self.runtime_files() == self.runtime_baseline, 'Module save must not publish runtime'
        self.key('Enter'); self.key('Home', 'Down')
        self.contains('Selected: 2 · 9.9.9.9')
        self.capture('07b-dns-second-row-before-tab')
        self.key('Tab')
        self.contains('Focus: actions')
        self.contains('Selected: 2 · 9.9.9.9')
        self.capture('07c-dns-second-row-after-tab')
        self.key('BTab', 'Home', 'e'); self.contains('Address')
        self.key('End'); self.type('x'); self.key('BSpace', 'C-s')
        self.key('c', '-', 'D'); self.contains('Remove native entry'); self.key('BTab', 'Enter')
        self.key('Escape', 'C-s')
        self.wait(lambda: len(self.source(a, 'dns')['dns']['servers']) == 2, 'DNS collection copy reorder remove')
        self.capture('08-dns-collection-edited')
        self.module('routing'); self.key('Enter'); self.contains('Routing rules')
        self.key('e'); self.contains('Rule label')
        self.key('Tab', 'End'); self.type(' edited'); self.key('C-s')
        self.key('c', 'Down', 'e'); self.contains('Rule label')
        self.key('Tab', 'End'); self.type(' copy'); self.key('C-s')
        self.key('+'); self.key('Escape', 'F2'); self.contains('Initial rule edited copy')
        self.capture('09-routing-copy-reordered-json')
        self.key('F2', 'C-s')
        self.wait(lambda: len(self.source(a, 'routing')['routing']['rules']) == 2, 'Routing source saved')
        routing = self.source(a, 'routing')['routing']['rules']
        assert all(rule['futureNative'] == 42 for rule in routing)
        assert routing[0]['ruleTag'].endswith('copy')
        self.key('Enter', 'D'); self.contains('Remove native entry'); self.key('BTab', 'Enter', 'Escape', 'C-s')
        self.wait(lambda: len(self.source(a, 'routing')['routing']['rules']) == 1, 'Routing deletion saved')
        self.page('Profiles'); self.content(); self.profile_cursor(a); self.key('e'); self.contains('Module Edit: none')
        before = self.source(a, 'dns')
        self.module('dns'); self.contains('Source read-only')
        self.key('F2'); self.type('q?invalid'); self.key('C-s')
        assert self.source(a, 'dns') == before
        self.capture('10-modules-readonly')
        all_sources = {key: self.rpc('module.get', profile_id=a, module=key).get('text') for key in MODULES}
        for key in MODULES:
            self.module(key); self.contains('Source read-only')
            self.key('F2'); self.contains('JSON'); self.key('F2')
        assert {key: self.rpc('module.get', profile_id=a, module=key).get('text') for key in MODULES} == all_sources
        self.capture('10b-all-15-modules-readonly')
        self.page('Profiles'); self.content(); self.profile_cursor(a); self.key('R'); self.contains('rename Profile')
        self.key('End', 'Left'); self.type('Z'); self.key('BSpace', 'F1')
        self.contains('Help'); self.key('Escape'); self.type('?'); self.key('C-s')
        self.wait(lambda: self.rpc('profile.get', profile_id=a)['name'] == 'Alph?a', 'Profile rename with Edit off')
        self.capture('11-profile-rename-without-edit')
        self.subscription_task()
        self.page('Nodes'); self.content(); self.capture('16-nodes-80x24')
        self.page('Logs'); self.content(); self.wait(lambda: 'UI process fixture' in self.grid() or 'XRM_FAKE_CORE_LOG_' in self.grid(), 'actual process log body')
        self.key('p', 'Home'); self.contains('Paused')
        logs = self.capture('17-logs-paused')
        assert 'Viewport' not in logs and '↑/↓ history' not in logs
        self.key('p'); self.contains('Following')
        self.page('Config'); self.content(); self.capture('18-config-cores')
        self.page('Profiles'); self.content(); self.profile_cursor(a); self.key('R'); self.contains('rename Profile')
        self.key('End'); self.type(' unsaved')
        self.tmux('resize-window', '-t', 'xrm', '-x', '120', '-y', '40'); time.sleep(.15)
        self.capture('19-form-120x40')
        self.tmux('resize-window', '-t', 'xrm', '-x', '60', '-y', '18'); self.contains('Terminal too small')
        self.capture('20-small-terminal-protection')
        self.tmux('resize-window', '-t', 'xrm', '-x', '180', '-y', '48'); self.contains('unsaved')
        self.capture('21-form-180x48')
        self.tmux('resize-window', '-t', 'xrm', '-x', '80', '-y', '24'); self.contains('unsaved')
        self.capture('22-restored-draft-80x24')
        self.key('C-s')
        self.wait(lambda: self.rpc('profile.get', profile_id=a)['name'].endswith(' unsaved'), 'resized draft saved')
        assert self.runtime_files() == self.runtime_baseline, 'UI edits must not republish runtime'
        self.record('verification', {'status': 'PASS', 'scenarios': 'seven pages; form input/help/errors; Use/Edit/readonly; DNS/routing native collections; subscription toast; Logs; resize; RPC and runtime-file facts',
                                     'real_xray': 'SKIP: process fixture only; real core verification is a separate gate', 'G5': 'PENDING user acceptance'})

    def subscription_task(self):
        payload = b'vless://11111111-1111-4111-8111-111111111111@127.0.0.1:12345?encryption=none&security=none&type=tcp#Fixture\n'
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(inner):
                inner.send_response(200); inner.send_header('Content-Type', 'text/plain'); inner.end_headers(); inner.wfile.write(payload)
            def log_message(inner, *args):
                pass
        self.http = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.http.serve_forever, daemon=True).start()
        url = f'http://127.0.0.1:{self.http.server_port}/fixture'
        self.page('Subscriptions'); self.content(); self.key('a'); self.contains('Add subscription')
        self.type('Local fixture'); self.key('Tab'); self.type(url)
        self.key('BTab', 'End'); self.type(' updated')
        self.capture('12-subscription-multifield')
        self.key('C-s')
        self.wait(lambda: len(self.rpc('subscription.list')) == 1, 'Subscription created')
        source = self.rpc('subscription.list')[0]
        assert source['name'] == 'Local fixture updated'
        self.contains('Local fixture updated')
        self.key('u')
        self.wait(lambda: self.rpc('subscription.list')[0].get('node_count', 0) == 1, 'Subscription actual node refresh', timeout=10)
        self.contains('completed')
        visible = self.capture('13-toast-completed')
        assert 'completed' in '\n'.join(visible.splitlines()[2:12]), visible
        before = self.tmux('display-message', '-p', '-t', 'xrm', '#{cursor_x},#{cursor_y}')
        self.wait(lambda: 'completed' not in self.grid(), 'Toast automatically disappears', timeout=5)
        after = self.tmux('display-message', '-p', '-t', 'xrm', '#{cursor_x},#{cursor_y}')
        self.capture('14-toast-expired')
        assert before == after, 'Toast changed input cursor/focus'
        assert self.runtime_files() == self.runtime_baseline, 'Subscription refresh changed runtime files'
        self.key('e'); self.contains('Edit subscription')
        self.key('End'); self.type(' draft')
        self.daemon.terminate(); self.daemon.wait(timeout=5)
        self.key('C-s')
        self.contains('draft')
        self.capture('15-save-failure-draft-retained')
        # Restart only our daemon with the same isolated store. The previous fake
        # core is stopped by xrmd shutdown; no production process is involved.
        self.daemon = subprocess.Popen([str(self.build / 'xrmd'), '--foreground', '--socket', str(self.sock), '--database', str(self.temp / 'state.sqlite')], env=self.env, stdout=self.server_log, stderr=subprocess.STDOUT)
        self.wait(lambda: self.sock.exists() and self.daemon.poll() is None, 'daemon restart')
        time.sleep(.15)
        self.key('C-s'); self.wait(lambda: self.rpc('subscription.list')[0]['name'].endswith(' draft'), 'Failed draft retried after reconnect')
        # Restore process/log fixture for remaining log-reader tests.
        self.operation('connection.connect', profile_id=self.profile_a)
        self.runtime_baseline = self.runtime_files()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=Path('out/dev'))
    parser.add_argument('--output', type=Path, default=Path('/tmp/xrm-rewrite-terminal-evidence'))
    args = parser.parse_args()
    harness = None
    try:
        harness = Harness(args.build, args.output)
        harness.run()
    except Exception:
        if harness:
            try:
                harness.capture('failure-final')
            except Exception:
                pass
            harness.record('failure', traceback.format_exc())
        raise
    finally:
        if harness:
            harness.close()
    print('Real terminal regression passed; evidence:', args.output)


if __name__ == '__main__':
    main()
