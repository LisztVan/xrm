#!/usr/bin/env python3
"""Isolated XRM IPC/HTTP diagnostic replay; no shared sockets, DB, or core."""
import http.server
import json
import os
import pathlib
import socket
import subprocess
import tempfile
import threading
import time

project = pathlib.Path(__file__).resolve().parents[4]
server_binary = project / 'build/usability-diagnostics-927a/xrmd'
output = pathlib.Path(__file__).with_name('live_rpc.json')
state = {'status': 200, 'body': 'vless://00000000-0000-0000-0000-000000000001@127.0.0.1:19090?encryption=none&security=none&type=tcp#Fixture\n'}
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = state['body'].encode()
        self.send_response(state['status'])
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_):
        pass

http = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=http.serve_forever, daemon=True)
thread.start()
sequence = 0
observed = {}
with tempfile.TemporaryDirectory(prefix='xrm-uxr07-diagnostics-') as directory:
    root = pathlib.Path(directory)
    env = os.environ.copy()
    for key, child in [('HOME','home'), ('XDG_RUNTIME_DIR','run'), ('XDG_STATE_HOME','state'), ('XDG_DATA_HOME','data'), ('XDG_CONFIG_HOME','config')]:
        path = root / child
        path.mkdir()
        env[key] = str(path)
    endpoint = root / 'run/xrmd.sock'
    database = root / 'state/server.sqlite'
    log_file = (root / 'xrmd.log').open('w')
    def start():
        proc = subprocess.Popen([str(server_binary), '--foreground', '--socket', str(endpoint), '--database', str(database)], env=env, stdout=log_file, stderr=subprocess.STDOUT)
        for _ in range(200):
            if endpoint.exists():
                return proc
            if proc.poll() is not None:
                raise RuntimeError(f'xrmd exited {proc.returncode}')
            time.sleep(.025)
        raise RuntimeError('xrmd socket did not open')
    def stop(proc):
        proc.terminate()
        proc.wait(timeout=10)
        if endpoint.exists():
            endpoint.unlink()
    daemon = start()
    def rpc(method, params=None):
        global sequence
        sequence += 1
        request = {'v': 1, 'id': sequence, 'method': method, 'params': params or {}}
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.connect(str(endpoint))
            sock.sendall((json.dumps(request) + '\n').encode())
            content = bytearray()
            while not content.endswith(b'\n'):
                part = sock.recv(65536)
                if not part:
                    raise RuntimeError('IPC closed without response')
                content.extend(part)
        response = json.loads(content)
        if response['id'] != sequence:
            raise RuntimeError('mismatched IPC response ID')
        return response
    def result(method, params=None):
        response = rpc(method, params)
        if not response['ok']:
            raise RuntimeError(f'{method}: {response["error"]}')
        return response['result']
    def refresh(source):
        accepted = result('subscription.refresh', {'source_ids': [source]})
        for _ in range(300):
            operation = result('operation.get', {'operation_id': accepted['operation_id']})
            if operation['status'] != 'running':
                return operation
            time.sleep(.025)
        raise RuntimeError('refresh operation timed out')
    try:
        rejected = rpc('subscription.add', {'name': 'Malformed', 'url': 'file:///secret'})
        assert not rejected['ok'] and rejected['error']['code'] == 'invalid_argument'
        assert rejected['error']['subject'] == 'url'
        observed['url_preflight'] = rejected['error']
        invalid_core = rpc('core.register', {'path': '/bin/true', 'name': 'Invalid core fixture'})
        assert not invalid_core['ok'] and invalid_core['error']['code'] == 'core_check_failed'
        observed['invalid_core_binary'] = invalid_core['error']
        missing_core_profile = result('profile.create', {'name': 'No core fixture'})['id']
        no_core_check = result('config.check', {'profile_id': missing_core_profile})
        for _ in range(300):
            no_core_operation = result('operation.get', {'operation_id': no_core_check['operation_id']})
            if no_core_operation['status'] != 'running':
                break
            time.sleep(.025)
        assert no_core_operation['status'] == 'failed' and no_core_operation['error']['code'] == 'not_found', no_core_operation
        observed['profile_without_core'] = no_core_operation['error']
        url = f'http://127.0.0.1:{http.server_address[1]}/nodes?token=private_fixture_token'
        source = result('subscription.add', {'name': 'Loopback fixture', 'url': url, 'auto_refresh': False})['id']
        first = refresh(source)
        assert first['status'] == 'succeeded' and not first['result']['failed']
        summary = result('subscription.list')[0]
        assert summary['node_count'] == 1 and summary['last_result'] == 'success'
        first_success = summary['last_success']
        observed['success'] = {'last_result': summary['last_result'], 'node_count': summary['node_count'], 'last_success_set': first_success is not None}
        state['status'] = 503
        failed = refresh(source)
        assert failed['status'] == 'succeeded' and failed['result']['failed']
        error = failed['result']['failed'][0]['error']
        summary = result('subscription.list')[0]
        assert summary['last_result'] == 'failed' and summary['node_count'] == 1
        assert summary['last_success'] == first_success
        assert summary['last_error_stage'] == 'http' and '503' in summary['last_error']
        assert '\x00' not in error['message'] and 'private_fixture_token' not in error['message']
        observed['http_503'] = {'error': error, 'last_result': summary['last_result'], 'last_error': summary['last_error'], 'last_error_stage': summary['last_error_stage'], 'node_count': summary['node_count'], 'last_success_preserved': summary['last_success'] == first_success}
        stop(daemon)
        daemon = start()
        restarted = result('subscription.list')[0]
        assert restarted['last_error'] == summary['last_error'] and restarted['node_count'] == 1
        observed['restart_persistence'] = {'last_error': restarted['last_error'], 'last_error_stage': restarted['last_error_stage'], 'node_count': restarted['node_count']}
        state['status'] = 200
        state['body'] += 'not-a-node\n'
        partial = refresh(source)
        assert partial['status'] == 'succeeded' and partial['result']['completed'][0]['partial']
        partial_result = partial['result']['completed'][0]
        summary = result('subscription.list')[0]
        assert summary['last_result'] == 'partial' and summary['last_error_stage'] == 'parse'
        observed['partial'] = {'first_issue': partial_result.get('first_issue'), 'last_error': summary['last_error'], 'last_error_stage': summary['last_error_stage'], 'node_count': summary['node_count']}
        state['body'] = '<html>login required</html>'
        invalid = refresh(source)
        assert invalid['status'] == 'succeeded' and invalid['result']['failed']
        summary = result('subscription.list')[0]
        assert summary['node_count'] == 1 and summary['last_success'] == first_success
        observed['invalid_body'] = {'error': invalid['result']['failed'][0]['error'], 'last_error': summary['last_error'], 'last_error_stage': summary['last_error_stage'], 'node_count': summary['node_count'], 'last_success_preserved': summary['last_success'] == first_success}
        state['body'] = 'vless://00000000-0000-0000-0000-000000000001@127.0.0.1:19090?encryption=none&security=none&type=tcp#Fixture\n'
        restored = refresh(source)
        assert restored['status'] == 'succeeded'
        summary = result('subscription.list')[0]
        assert summary['last_result'] == 'success' and summary.get('last_error') is None
        observed['restored'] = {'last_result': summary['last_result'], 'last_error': summary.get('last_error'), 'node_count': summary['node_count']}
        refused_url = 'http://127.0.0.1:1/nodes?token=private_fixture_token'
        refused_source = result('subscription.add', {'name': 'Refused fixture', 'url': refused_url, 'auto_refresh': False})['id']
        refused = refresh(refused_source)
        refused_error = refused['result']['failed'][0]['error']
        refused_summary = next(row for row in result('subscription.list') if row['id'] == refused_source)
        assert '\x00' not in refused_error['message'] and '\x00' not in refused_summary['last_error']
        assert 'private_fixture_token' not in refused_error['message'] and 'private_fixture_token' not in refused_summary['last_error']
        assert refused_summary['last_error_stage'] == 'download'
        observed['connection_refused'] = {'error': refused_error, 'last_error': refused_summary['last_error'], 'last_error_stage': refused_summary['last_error_stage']}
        before = result('runtime.status')
        stale = rpc('connection.disconnect', {'expected_run_id': 'no-longer-current'})
        after = result('runtime.status')
        assert not stale['ok'] and stale['error']['code'] == 'conflict' and before == after
        observed['stale_stop'] = {'error': stale['error'], 'runtime_unchanged': before == after}
    finally:
        if daemon.poll() is None:
            stop(daemon)
        log_file.close()
http.shutdown()
http.server_close()
output.write_text(json.dumps(observed, ensure_ascii=False, indent=2) + '\n')
print(output)
