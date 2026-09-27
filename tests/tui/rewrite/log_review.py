#!/usr/bin/env python3
"""R6/V10: real Xray logs and Help actions in an isolated tmux/daemon lab.

Captures are actual tmux cells/ANSI and HTML rerenders. Only loopback traffic;
no installed daemon, subscription, system route or unrelated tmux is touched.
"""
import argparse
import hashlib
import http.server
import json
import re
from pathlib import Path
import socket
import subprocess
import threading

from terminal_regression import Harness, MODULES


class LogHarness(Harness):
    def module(self, name):
        self.page('Profiles')
        self.page('Modules')
        self.key('Enter')
        self.contains('SecondaryNav')
        position = re.search(r'(\d+)/15', self.grid().splitlines()[1])
        current = int(position.group(1)) - 1 if position else 0
        for _ in range((MODULES.index(name) - current) % len(MODULES)):
            self.key('Tab')
        self.key('Enter')
        self.contains(name)
        self.contains('Content')

    def resize(self, width, height):
        self.tmux('resize-window', '-t', 'xrm', '-x', str(width), '-y', str(height))
        def drawn():
            lines = self.grid().splitlines()
            return (len(lines) == height and lines[-1].rstrip().endswith('F1 Help & actions')
                    and len(lines[-1].rstrip()) >= width - 2
                    and ('SUBSYSTEM' in lines[3]) == (width >= 115))
        self.wait(drawn, f'{width}x{height} application redraw')

    def help_action(self, labels, capture=None):
        if isinstance(labels, str):
            labels = [labels]
        before = self.rpc('runtime.status')
        self.key('F1')
        self.contains('Tasks · arrows choose')
        after = self.rpc('runtime.status')
        assert (before.get('pid'), before['state']) == (after.get('pid'), after['state']), 'F1 performed a runtime action'
        if capture:
            self.capture(capture)
        for _ in range(30):
            active = next((line for line in self.grid().splitlines()
                           if re.search(r'>\s+\S', line)), '')
            if any(label in active for label in labels):
                self.record('help_execute', active.strip())
                self.key('Enter')
                self.wait(lambda: 'Tasks · arrows choose' not in self.grid(), 'Help action consumed Enter')
                return
            self.key('Down')
        self.capture('failure-help-' + re.sub(r'[^a-z0-9]+', '-', labels[0].lower()))
        raise AssertionError(f'Help action missing: {labels}')

    def loopback(self, port, suffix='proof'):
        target = f'http://127.0.0.1:{self.http.server_port}/{suffix}'
        result = subprocess.run(['curl', '--silent', '--show-error', '--fail', '--max-time', '8',
                                 '--noproxy', '', '--proxy', f'socks5h://127.0.0.1:{port}', target],
                                check=True, capture_output=True, text=True)
        assert result.stdout == 'xrm-review-real-loopback\n'
        self.record('loopback', {'url': target, 'response': result.stdout})

    def run(self, xray):
        self.record('terminal_environment', {key: self.env.get(key) for key in ['TERM', 'COLORTERM', 'NO_COLOR']})
        version = subprocess.check_output([str(xray), 'version'], text=True)
        assert '26.9.9' in version, version
        class Response(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = b'xrm-review-real-loopback\n'
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *_):
                pass
        self.http = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Response)
        threading.Thread(target=self.http.serve_forever, daemon=True).start()
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        core = self.rpc('core.register', path=str(xray), name='Fixed Xray 26.9.9')
        profile = self.rpc('profile.create', name='Real log review', core_id=core['id'], preset='custom')['id']
        for key, value in {
            'log': {'loglevel': 'debug'},
            'inbounds': [{'tag': 'review-socks', 'listen': '127.0.0.1', 'port': port,
                          'protocol': 'socks', 'settings': {'auth': 'noauth'}}],
            'outbounds': [{'tag': 'review-direct', 'protocol': 'freedom', 'settings': {}}],
            'dns': {'servers': ['localhost']},
            'routing': {'rules': [{'network': 'tcp', 'outboundTag': 'review-direct'}]},
        }.items():
            self.rpc('module.save', profile_id=profile, module=key, text=json.dumps({key: value}))
            self.rpc('module.include', profile_id=profile, module=key, included=True)
        assert self.rpc('runtime.status')['state'] == 'stopped'
        assert not self.runtime_files()
        self.start_tui(profile)
        self.content()
        self.help_action('Connect saved', '01-overview-help-before-connect')
        self.wait(lambda: self.rpc('runtime.status')['state'] == 'running', 'Xray running', timeout=12)
        self.loopback(port)
        with socket.create_connection(('127.0.0.1', port), timeout=3) as connection:
            connection.sendall(b'GET /invalid-socks HTTP/1.0\r\n\r\n')
        self.loopback(port, 'second')
        status = self.rpc('runtime.status')
        pid = status['pid']
        runtime = self.runtime_files()
        confdir = Path(status['active']['confdir'])
        files = sorted(path.name for path in confdir.glob('*.json'))
        assert 'config.json' not in files and len(files) >= 5
        self.page('Logs')
        self.content()
        self.contains('Following')
        self.contains('SOURCE')
        self.wait(lambda: 'Connect Real log review:' not in self.grid(), 'connect notification expires')
        self.help_action(['Pause reading', 'Pause / follow'], '02-logs-help')
        self.contains('Paused')
        self.key('Home')
        for width, height in [(80, 24), (120, 40), (180, 48)]:
            self.resize(width, height)
            screen = self.capture(f'03-logs-{width}x{height}')
            assert 'TIME' in screen and 'SOURCE' in screen and 'LEVEL' in screen
            assert 'CURSOR' not in screen and ' · cursor ' not in screen
            assert 'Paused' in screen
        self.resize(80, 24)
        frozen = self.grid().splitlines()[4:-4]
        self.loopback(port, 'while-paused')
        self.wait(lambda: 'new text lines' in self.grid(), 'new logs buffered while paused')
        assert self.grid().splitlines()[4:-4] == frozen, 'Paused output moved'
        self.capture('04-paused-with-new-output-80x24')
        self.resize(120, 40)
        self.help_action('Raw log details')
        self.contains('Transport chunk')
        self.contains('Line within chunk')
        self.capture('05-original-log-detail-120x40')
        self.key('Escape')
        self.help_action(['Resume following', 'Pause / follow'])
        self.contains('Following')
        self.capture('06-following-120x40')
        after = self.rpc('runtime.status')
        assert after['pid'] == pid and after['state'] == 'running', 'Logs changed runtime'
        assert self.runtime_files() == runtime, 'Logs modified runtime files'
        raw = self.rpc('logs.read', cursor=0, limit=1048576)
        (self.output / 'real-xray-output.json').write_text(json.dumps(raw, ensure_ascii=False, indent=2))
        self.page('Overview')
        self.content()
        self.help_action('Stop core', '07-overview-help-before-stop')
        self.wait(lambda: self.rpc('runtime.status')['state'] == 'stopped', 'Help Stop completed')
        self.page('Logs')
        self.content()
        self.contains('Following')
        self.resize(80, 24)
        self.daemon.terminate()
        self.daemon.wait(timeout=8)
        self.contains('Cached · disconnected', timeout=10)
        offline = self.capture('08-cached-disconnected-80x24')
        assert 'Following' not in offline and 'TIME' in offline
        self.daemon = subprocess.Popen([str(self.build / 'xrmd'), '--foreground', '--socket', str(self.sock),
                                       '--database', str(self.temp / 'state.sqlite')], env=self.env,
                                      stdout=self.server_log, stderr=subprocess.STDOUT)
        self.wait(lambda: self.sock.exists(), 'restarted isolated daemon')
        self.contains('Following', timeout=12)
        self.capture('09-restored-80x24')
        assert self.rpc('runtime.status')['state'] == 'stopped', 'Reconnect replayed a runtime write'
        self.record('verification', {'status': 'PASS', 'xray_version': version,
                    'xray_sha256': hashlib.sha256(xray.read_bytes()).hexdigest(),
                    'xrmd_sha256': hashlib.sha256((self.build / 'xrmd').read_bytes()).hexdigest(),
                    'profile': profile, 'running_pid': pid, 'native_confdir': str(confdir),
                    'native_files': files, 'screens': ['80x24', '120x40', '180x48'],
                    'actions_via_help': ['Connect saved', 'Pause / follow', 'Raw log details', 'Stop core'],
                    'logs_side_effects': 'same PID and runtime file digests',
                    'limits': 'Source fixture seeded by RPC; no OS IME or native emulator pixel capture; '
                              'control/UTF8 chunk boundaries covered by unit and real-pipe tests, not fabricated in Xray output'})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('out/dev'))
    parser.add_argument('--xray', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('docs/evidence/review-remediation/logs'))
    args = parser.parse_args()
    harness = LogHarness(args.build, args.output)
    try:
        harness.run(args.xray.resolve())
    finally:
        harness.close()
