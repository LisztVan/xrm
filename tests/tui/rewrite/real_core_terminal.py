#!/usr/bin/env python3
"""Real TUI native forms -> xrmd -> fixed Xray -> loopback SOCKS traffic."""
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import socket
import subprocess
import threading
from log_review import LogHarness

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=Path('out/real-core'))
parser.add_argument('--xray', type=Path, required=True)
parser.add_argument('--output', type=Path, default=Path('out/real-core/real-terminal'))
args = parser.parse_args()
xray = args.xray.resolve()
version = subprocess.check_output([str(xray), 'version'], text=True)
assert '26.9.9' in version, version
h = LogHarness(args.build, args.output)
try:
    class Response(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = b'xrm-real-component-loopback\n'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        def log_message(self, *_): pass
    h.http = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Response)
    threading.Thread(target=h.http.serve_forever, daemon=True).start()
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    core = h.rpc('core.register', path=str(xray), name='Fixed Xray')
    profile = h.rpc('profile.create', name='Real Loopback', core_id=core['id'], preset='custom')['id']
    for key, value in {
        'log': {'loglevel': 'warning'},
        'inbounds': [{'tag': 'socks-in', 'listen': '127.0.0.1', 'port': port,
                      'protocol': 'socks', 'settings': {'auth': 'noauth'}}],
        'outbounds': [{'tag': 'direct', 'protocol': 'freedom', 'settings': {}}],
        'dns': {'servers': ['127.0.0.1']},
        'routing': {'rules': [{'ruleTag': 'Loopback', 'network': 'tcp', 'outboundTag': 'direct'}]},
    }.items():
        h.rpc('module.save', profile_id=profile, module=key, text=json.dumps({key: value}))
        h.rpc('module.include', profile_id=profile, module=key, included=True)
    baseline = h.runtime_files()
    h.start_tui(profile)
    h.page('Profiles'); h.content(); h.help_action('Allow module Edit'); h.contains('Edit')
    h.module('dns'); h.key('Enter'); h.help_action('Edit entry'); h.contains('Address')
    h.key('Home'); h.tmux('send-keys', '-t', 'xrm', '-N', '30', 'Delete'); h.type('localhost')
    h.capture('01-dns-form')
    h.help_action('Add to draft'); h.key('Escape'); h.help_action('Save module source')
    h.wait(lambda: h.source(profile, 'dns')['dns']['servers'] == ['localhost'], 'TUI DNS save')
    h.module('routing'); h.key('Enter'); h.help_action('Edit entry'); h.contains('Rule label')
    # ruleTag already exists: Tab enters the explicit value without toggling
    # its presence checkbox. Unset fields would require Space to opt in first.
    h.key('Tab', 'Home'); h.tmux('send-keys', '-t', 'xrm', '-N', '30', 'Delete'); h.type('TUI loopback')
    h.capture('02-routing-form')
    h.help_action('Add to draft'); h.key('Escape'); h.help_action('Save module source')
    h.wait(lambda: h.source(profile, 'routing')['routing']['rules'][0]['ruleTag'] == 'TUI loopback', 'TUI routing save')
    assert h.runtime_files() == baseline
    assert h.rpc('runtime.status')['state'] == 'stopped'
    h.page('Overview'); h.content(); h.help_action('Connect saved')
    h.wait(lambda: h.rpc('runtime.status')['state'] == 'running', 'actual Xray running', timeout=12)
    h.contains('Runtime  ACTUAL'); h.contains('running  Xray')
    h.capture('03-real-xray-running')
    target = f'http://127.0.0.1:{h.http.server_port}/proof'
    reply = subprocess.run(['curl', '--silent', '--show-error', '--fail', '--max-time', '8',
                            '--noproxy', '', '--proxy', f'socks5h://127.0.0.1:{port}', target],
                           check=True, capture_output=True, text=True)
    assert reply.stdout == 'xrm-real-component-loopback\n'
    status = h.rpc('runtime.status')
    confdir = Path(status['active']['confdir'])
    files = sorted(p.name for p in confdir.glob('*.json'))
    assert 'config.json' not in files and len(files) >= 5, files
    stats = h.rpc('runtime.stats', watch=False)
    h.record('runtime_stats', stats)
    h.page('Logs'); h.content(); h.contains('SOURCE')
    h.help_action(['Pause reading', 'Pause / follow'])
    h.contains('Paused'); h.capture('04-real-xray-logs')
    assert h.rpc('runtime.status')['pid'] == status['pid']
    h.help_action('Raw log details'); h.contains('Transport chunk')
    h.capture('05-real-xray-log-detail'); h.key('Escape')
    h.record('real_core_proof', {'version': version, 'sha256': hashlib.sha256(xray.read_bytes()).hexdigest(),
             'profile': profile, 'pid': status['pid'], 'confdir': str(confdir), 'files': files,
             'curl_stdout': reply.stdout, 'route': target, 'socks_port': port,
             'dns': h.source(profile, 'dns'), 'routing': h.source(profile, 'routing')})
    h.page('Overview'); h.key('q')
    assert h.rpc('runtime.status')['pid'] == status['pid'], 'TUI exit must retain core'
    h.wait(lambda: not h.tmux('list-panes', '-t', 'xrm', check=False).strip(), 'TUI exited')
    h.start_tui(profile); h.content(); h.help_action('Stop core')
    h.contains('Stop core'); h.key('BTab', 'Enter')
    h.wait(lambda: h.rpc('runtime.status')['state'] == 'stopped', 'Help stop actual Xray')
    h.capture('06-real-xray-stopped')
    assert h.rpc('runtime.status')['state'] == 'stopped'
finally:
    h.close()
print('Real Xray TUI forms and SOCKS loopback passed:', args.output)
