#!/usr/bin/env python3
"""UXR-12/13 against an isolated xrmd, local HTTP fixture and real CLI pipes."""
import argparse
import hashlib
import http.server
import json
import os
import pathlib
import pty
import select
import shlex
import signal
import subprocess
import tempfile
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--xrm', type=pathlib.Path, required=True)
parser.add_argument('--xrmd', type=pathlib.Path, required=True)
parser.add_argument('--xray', type=pathlib.Path, required=True)
parser.add_argument('--evidence', type=pathlib.Path, required=True)
args = parser.parse_args()
for key in ('xrm', 'xrmd', 'xray'):
    setattr(args, key, getattr(args, key).resolve())
args.evidence.mkdir(parents=True, exist_ok=True)
trace = []


def check(ok, why):
    if not ok:
        raise AssertionError(why)


def record(label, argv, result):
    trace.append({'label': label, 'argv': argv, 'code': result.returncode,
                  'stdout': result.stdout, 'stderr': result.stderr})
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/http-404':
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if self.path == '/bad-html':
            body = b'<html><body>not a subscription</body></html>'
        else:
            body = (b'vless://00000000-0000-0000-0000-000000000001@127.0.0.1:19090'
                    b'?encryption=none&security=none&type=tcp#Loopback%20fixture\n'
                    b'invalid://unsupported-fixture\n')
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


with tempfile.TemporaryDirectory(prefix='xrm-uxr-cli-') as temporary:
    root = pathlib.Path(temporary)
    env = {**os.environ, 'HOME': str(root / 'home'),
           'XDG_RUNTIME_DIR': str(root / 'runtime'), 'XDG_CONFIG_HOME': str(root / 'config'),
           'XDG_STATE_HOME': str(root / 'state'), 'XDG_DATA_HOME': str(root / 'data'),
           'NO_COLOR': '1'}
    for name in ('home', 'runtime', 'config', 'state', 'data'):
        (root / name).mkdir()
    socket = root / 'daemon.sock'
    prefix = [str(args.xrm), '--socket', str(socket)]
    fixture = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=fixture.serve_forever, daemon=True).start()
    trace.append({'label': 'fixture', 'socket': str(socket), 'database': str(root / 'db/state.sqlite'),
                  'http_port': fixture.server_address[1], 'core_sha256': digest(args.xray),
                  'xrm_sha256': digest(args.xrm), 'xrmd_sha256': digest(args.xrmd)})

    def run(label, args_, expected=0, input_=None):
        argv = prefix + args_
        result = record(label, args_, subprocess.run(argv, env=env, text=True, input=input_,
                                                     capture_output=True, timeout=25))
        check(result.returncode == expected,
              f'{label}: expected {expected}, got {result.returncode}: {result.stderr!r}')
        return result

    def machine(label, args_, expected=0):
        result = run(label, ['--json'] + args_, expected)
        check(not result.stderr, f'{label}: machine output leaked to stderr')
        return json.loads(result.stdout)

    def tty(label, args_):
        master, slave = pty.openpty()
        try:
            process = subprocess.Popen(prefix + args_, env=env, stdin=slave, stdout=slave,
                                       stderr=subprocess.PIPE)
            os.close(slave)
            output = bytearray()
            while True:
                readable, _, _ = select.select([master], [], [], .1)
                if readable:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    output.extend(chunk)
                if process.poll() is not None and not readable:
                    break
            stderr = process.stderr.read().decode(errors='replace')
            process.wait(timeout=10)
            decoded = output.decode(errors='replace').replace('\r\n', '\n')
            trace.append({'label': label, 'argv': args_, 'code': process.returncode,
                          'stdout': decoded, 'stderr': stderr, 'tty': True})
            check(process.returncode == 0 and not stderr, f'{label}: TTY failure')
            return decoded
        finally:
            os.close(master)

    daemon_log = (root / 'daemon.log').open('w')
    daemon = subprocess.Popen([str(args.xrmd), '--foreground', '--socket', str(socket),
                               '--database', str(root / 'db/state.sqlite')], env=env,
                              stdout=daemon_log, stderr=daemon_log)
    children = []
    try:
        for _ in range(100):
            if socket.exists():
                break
            check(daemon.poll() is None, 'isolated xrmd exited before socket appeared')
            time.sleep(.05)
        check(socket.exists(), 'isolated xrmd socket missing')

        empty_logs = run('non-TTY empty logs', ['logs'])
        check(not empty_logs.stdout and not empty_logs.stderr, 'empty logs pipe output changed')
        check('No log entries' in tty('TTY empty logs', ['logs']),
              'TTY empty logs lacks a visible empty state')

        core = machine('fixed core registration', ['core', 'register', str(args.xray), '--name', 'Fixed Xray'])
        bad_core = run('invalid executable registration',
                       ['core', 'register', '/bin/true', '--name', 'Not Xray'], 1)
        check('executable' in bad_core.stderr.lower() and 'candidate configuration' not in bad_core.stderr,
              'invalid binary still misclassified as saved configuration')
        machine('profile create', ['profile', 'create', '--name', 'Home'])
        selected_core = run('human core use receipt', ['core', 'use', 'Home', core['id']])
        check('running instance unchanged' in selected_core.stdout and
              'checked_only' not in selected_core.stdout, 'core use leaked internal effect')
        invalid_url = run('invalid subscription URL syntax',
                          ['sub', 'add', '--name', 'Invalid URL', '--url', 'not-a-url'], 1)
        check(not invalid_url.stdout and 'URL' in invalid_url.stderr and
              machine('no source after URL rejection', ['sub', 'list']) == [],
              'invalid URL was saved or lacked recovery')
        baseline = '{"dns":{"servers":["localhost"],"unknownFixture":{"keep":true}}}'
        file_path = root / 'module.json'
        file_path.write_text(baseline)
        run('config file input', ['config', 'edit', 'Home', 'dns', '--file', str(file_path)])
        source = machine('file source', ['config', 'show', 'Home', 'dns'])
        check(baseline in source.get('text', ''), 'file save lost unknown JSON or native text')
        replacement = '{"dns":{"servers":["1.1.1.1"],"unknownFixture":{"keep":0}}}'
        stdin_result = record('config stdin input', ['config', 'edit', 'Home', 'dns', '--file', '-'],
                              subprocess.run(prefix + ['config', 'edit', 'Home', 'dns', '--file', '-'],
                                             env=env, input=replacement, text=True,
                                             capture_output=True, timeout=25))
        check(stdin_result.returncode == 0, f'stdin save failed: {stdin_result.stderr}')
        source = machine('stdin source', ['config', 'show', 'Home', 'dns'])
        check(replacement in source.get('text', ''), 'stdin save lost unknown JSON or native text')
        run('legacy text input', ['config', 'edit', 'Home', 'dns', '--text', baseline])
        source = machine('before invalid inputs', ['config', 'show', 'Home', 'dns'])
        file_path.write_text('{"dns":{"servers":["fixture-secret-token"],}}')
        bad = run('invalid JSON file', ['config', 'edit', 'Home', 'dns', '--file', str(file_path)], 1)
        check('fixture-secret-token' not in bad.stderr and 'Invalid native module JSON at byte' in bad.stderr,
              'invalid JSON leaked content or lost parse location')
        run('missing JSON file', ['config', 'edit', 'Home', 'dns', '--file', str(root / 'missing.json')], 3)
        conflict = run('mutually exclusive JSON input',
                       ['config', 'edit', 'Home', 'dns', '--file', str(file_path), '--text', baseline], 108)
        check(not conflict.stdout and '--file' in conflict.stderr and '--text' in conflict.stderr,
              'conflicting input sources were not explained')
        check(machine('source unchanged after invalid input', ['config', 'show', 'Home', 'dns']) == source,
              'invalid local input changed Source')

        refused = 'http://127.0.0.1:1/unreachable'
        first = machine('source add unique', ['sub', 'add', '--name', 'Local fixture', '--url', refused, '--paused'])
        first_id = first['id']
        same_id_name = machine('source add ID-shaped name',
                               ['sub', 'add', '--name', first_id, '--url', refused, '--paused'])
        a = machine('source add duplicate A', ['sub', 'add', '--name', 'Duplicate', '--url', refused, '--paused'])
        b = machine('source add duplicate B', ['sub', 'add', '--name', 'Duplicate', '--url', refused, '--paused'])
        check(machine('subscription exact name', ['sub', 'show', 'Local fixture'])['subscription']['id'] == first_id,
              'unique name did not resolve')
        check(machine('subscription ID priority', ['sub', 'show', first_id])['subscription']['id'] == first_id,
              'exact ID lost precedence to a different source name')
        check(machine('subscription other ID', ['sub', 'show', same_id_name['id']])['subscription']['id'] == same_id_name['id'],
              'ID lookup failed')
        listing = run('non-TTY subscription list', ['sub', 'list'])
        check(any(line.startswith(first_id + '\tLocal fixture\t') for line in listing.stdout.splitlines()) and
              '\x1b' not in listing.stdout,
              'subscription TSV column order or non-TTY color changed')
        ambiguous = run('ambiguous subscription name', ['sub', 'show', 'Duplicate'], 1)
        check(a['id'] in ambiguous.stderr and b['id'] in ambiguous.stderr and not ambiguous.stdout,
              'ambiguous name did not offer both IDs')
        run('subscription edit by name', ['sub', 'edit', 'Local fixture', '--name', 'Renamed source'])
        check(machine('renamed source lookup', ['sub', 'show', 'Renamed source'])['subscription']['id'] == first_id,
              'edit by name did not modify intended source')
        missing = run('unknown subscription name', ['sub', 'show', 'No such source'], 1)
        check('sub list' in missing.stderr and not missing.stdout, 'unknown source recovery failed')

        failed = run('human subscription all failed', ['sub', 'update', 'Renamed source'], 4)
        check(not failed.stdout and 'network' in failed.stderr.lower() and 'not found' not in failed.stderr.lower(),
              'download failure had success stdout or wrong not-found reason')
        detail = run('failed subscription detail by name', ['sub', 'show', 'Renamed source'])
        check('Failure stage: download' in detail.stdout and 'Last error:' in detail.stdout,
              'persisted download cause unavailable in human detail')
        machine_failure = machine('JSON aggregate failure', ['sub', 'update', first_id], 4)
        check(machine_failure.get('failed') and machine_failure.get('completed') == [],
              'aggregate JSON failure schema changed')
        all_failed = run('all sources failed', ['sub', 'update'], 4)
        check(not all_failed.stdout and all_failed.stderr.count('Subscription update failed') >= 4,
              'batch total failure polluted stdout or omitted failed sources')
        fixture_port = fixture.server_address[1]
        for label, suffix, stage in [('HTTP 404', '/http-404', 'http'),
                                     ('HTML response', '/bad-html', 'parse')]:
            source_name = label + ' fixture'
            machine(label + ' add', ['sub', 'add', '--name', source_name,
                                     '--url', f'http://127.0.0.1:{fixture_port}{suffix}', '--paused'])
            observed = run(label + ' update', ['sub', 'update', source_name], 4)
            check(not observed.stdout and ('Stage: ' + stage) in observed.stderr,
                  label + ' failure stage was lost or stdout polluted')
            if label == 'HTTP 404':
                check('HTTP 404' in observed.stderr, 'HTTP status not in human cause')
        partial_url = 'http://127.0.0.1:%d/partial' % fixture.server_address[1]
        partial_id = machine('partial fixture add',
                             ['sub', 'add', '--name', 'Partial fixture', '--url', partial_url, '--paused'])['id']
        partial = run('human partial subscription', ['sub', 'update', 'Partial fixture'], 4)
        check(' partial' in partial.stdout and 'Partial subscription update' in partial.stderr,
              'partial update did not separate result and diagnosis')
        partial_detail = run('partial source detail', ['sub', 'show', 'Partial fixture'])
        check('Failure stage: parse' in partial_detail.stdout and 'unsupported_uri_feature' in partial_detail.stdout,
              'persisted partial issue unavailable in human detail')
        json_partial = machine('JSON aggregate partial', ['sub', 'update', partial_id], 4)
        check(json_partial['completed'][0]['partial'] and json_partial['completed'][0]['source_id'] == partial_id,
              'partial JSON envelope changed')
        check(machine('old node retained after partial', ['node', 'list'])['total'] >= 1,
              'partial fixture lost node inventory')
        old_nodes = {node['id'] for node in machine('before failed re-update', ['node', 'list'])['items']
                     if node.get('source_id') == partial_id}
        run('change source URL before failure', ['sub', 'edit', 'Partial fixture', '--url', refused])
        run('failed re-update', ['sub', 'update', 'Partial fixture'], 4)
        kept_nodes = {node['id'] for node in machine('after failed re-update', ['node', 'list'])['items']
                      if node.get('source_id') == partial_id}
        check(old_nodes and kept_nodes == old_nodes, 'failed source update cleared old nodes')
        run('remove by name', ['sub', 'remove', 'Renamed source'])

        for label, args_, needle in [
            ('TTY profile list', ['profile', 'list'], 'Home'),
            ('TTY profile show', ['profile', 'show', 'Home'], 'Home'),
            ('TTY core list', ['core', 'list'], 'Fixed Xray'),
            ('TTY sub list', ['sub', 'list'], 'Partial fixture'),
            ('TTY node list', ['node', 'list'], 'Loopback fixture'),
            ('TTY group list', ['group', 'list', 'Home'], 'No groups'),
            ('TTY status', ['status'], 'state:'),
            ('TTY logs help', ['logs', '--help'], 'Transport buffer cursor')]:
            check(needle in tty(label, args_), f'{label}: human context missing')
        check('Maximum log text bytes' in tty('TTY logs limits', ['logs', '--help']),
              'logs --limit units or default not explained')

        def watch(json_mode=False):
            words = (['--json'] if json_mode else []) + ['stats', '--watch']
            process = subprocess.Popen(prefix + words, env=env, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE)
            children.append(process)
            return process, words

        for label, json_mode in [('human head', False), ('JSON head', True)]:
            producer, words = watch(json_mode)
            consumer = subprocess.Popen(['head', '-n', '1'], stdin=producer.stdout,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            producer.stdout.close()
            head_out, head_err = consumer.communicate(timeout=8)
            producer.wait(timeout=8)
            producer_err = producer.stderr.read()
            trace.append({'label': label, 'producer_argv': words,
                          'producer_code': producer.returncode, 'consumer_argv': ['head', '-n', '1'],
                          'consumer_code': consumer.returncode, 'stdout': head_out.decode(),
                          'producer_stderr': producer_err.decode(), 'consumer_stderr': head_err.decode()})
            check(producer.returncode == consumer.returncode == 0 and not producer_err and not head_err,
                  f'{label}: stdout consumer close was not quiet')
            if json_mode:
                check(isinstance(json.loads(head_out), dict), 'JSON watch sample invalid')

        for pipefail in (False, True):
            command = ('set -o pipefail\n' if pipefail else 'set +o pipefail\n') + (
                f'{shlex.quote(str(args.xrm))} --socket {shlex.quote(str(socket))} '
                'stats --watch | head -n 1\n'
                'printf "producer=%s consumer=%s aggregate=%s\\n" '
                '"${PIPESTATUS[0]}" "${PIPESTATUS[1]}" "$?"\n')
            shell = subprocess.run(['bash', '-c', command], env=env, capture_output=True,
                                   text=True, timeout=12)
            trace.append({'label': 'shell head pipefail=' + str(pipefail), 'code': shell.returncode,
                          'stdout': shell.stdout, 'stderr': shell.stderr})
            check(shell.returncode == 0 and not shell.stderr and
                  'producer=0 consumer=0 aggregate=0' in shell.stdout,
                  'bash pipeline process statuses or pipefail changed')

        producer, words = watch(True)
        consumer = subprocess.Popen(['cat'], stdin=producer.stdout,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        producer.stdout.close()
        check(select.select([consumer.stdout], [], [], 5)[0], 'watch | cat did not flush')
        cat_first = consumer.stdout.readline()
        producer.send_signal(signal.SIGINT)
        cat_rest, cat_err = consumer.communicate(timeout=8)
        producer.wait(timeout=8)
        producer_err = producer.stderr.read()
        trace.append({'label': 'watch cat then SIGINT', 'producer_code': producer.returncode,
                      'consumer_code': consumer.returncode, 'stdout': (cat_first + cat_rest).decode(),
                      'producer_stderr': producer_err.decode(), 'consumer_stderr': cat_err.decode()})
        check(producer.returncode == consumer.returncode == 0 and not producer_err and not cat_err,
              'watch | cat changed normal pipe contract')

        with open('/dev/full', 'wb') as full:
            nonpipe = subprocess.run(prefix + ['--json', 'stats', '--watch'], env=env,
                                     stdout=full, stderr=subprocess.PIPE, timeout=8)
        trace.append({'label': 'stdout non-EPIPE', 'code': nonpipe.returncode,
                      'stderr': nonpipe.stderr.decode()})
        check(nonpipe.returncode == 3 and json.loads(nonpipe.stderr)['error']['code'] == 'io_error',
              'non-EPIPE stdout failure was swallowed')

        producer, words = watch(True)
        first_sample = producer.stdout.readline()
        check(first_sample and isinstance(json.loads(first_sample), dict), 'watch sample did not flush')
        producer.send_signal(signal.SIGINT)
        out, err = producer.communicate(timeout=8)
        trace.append({'label': 'watch SIGINT', 'producer_code': producer.returncode,
                      'stdout': (first_sample + out).decode(), 'stderr': err.decode()})
        check(producer.returncode == 0 and not err, 'watch SIGINT changed')

        producer, words = watch(True)
        first_sample = producer.stdout.readline()
        daemon.terminate()
        daemon.wait(timeout=8)
        out, err = producer.communicate(timeout=8)
        trace.append({'label': 'watch daemon disconnected', 'producer_code': producer.returncode,
                      'stdout': (first_sample + out).decode(), 'stderr': err.decode()})
        check(producer.returncode == 3 and json.loads(err)['error']['code'] == 'io_error',
              'daemon IPC failure was mistaken for stdout EPIPE')
    finally:
        for process in children:
            if process.poll() is None:
                process.kill()
                process.wait()
        if daemon.poll() is None:
            daemon.terminate()
            daemon.wait(timeout=8)
        daemon_log.close()
        fixture.shutdown()
        fixture.server_close()
        (args.evidence / 'trace.json').write_text(json.dumps(trace, ensure_ascii=False, indent=2) + '\n')

print(f'UXR-12/13: {len(trace)} isolated CLI observations passed')
