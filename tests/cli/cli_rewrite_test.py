#!/usr/bin/env python3
"""R10/R11: actual CLI processes, isolated daemon, stdout/stderr/exit evidence.

Usage: python3 tests/cli/cli_rewrite_test.py --xrm build/.../xrm --xrmd build/.../xrmd
Optional --evidence saves captures and --xray exercises successful connect/stop.
"""
import argparse
import json
import fcntl
import struct
import termios
import os
import pathlib
import pty
import re
import shlex
import select
import signal
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--xrm', required=True, type=pathlib.Path)
parser.add_argument('--xrmd', required=True, type=pathlib.Path)
parser.add_argument('--xray', type=pathlib.Path)
parser.add_argument('--evidence', type=pathlib.Path)
args = parser.parse_args()
args.xrm, args.xrmd = args.xrm.resolve(), args.xrmd.resolve()
records = []

def check(condition, message):
    if not condition:
        raise AssertionError(message)

with tempfile.TemporaryDirectory(prefix='xrm-cli-rewrite-') as temp:
    root = pathlib.Path(temp)
    env = {**os.environ, 'HOME': str(root / 'home'), 'XDG_RUNTIME_DIR': str(root / 'runtime'),
           'XDG_CONFIG_HOME': str(root / 'config'), 'XDG_STATE_HOME': str(root / 'state'),
           'XDG_DATA_HOME': str(root / 'data'), 'NO_COLOR': '1'}
    socket = root / 'daemon.sock'
    prefix = [str(args.xrm), '--socket', str(socket)]

    def run(words, code=0):
        result = subprocess.run(prefix + words, env=env, text=True, capture_output=True, timeout=12)
        records.append({'argv': words, 'code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
        check(result.returncode == code if code is not None else result.returncode != 0,
              f'{words}: exit {result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}')
        check('\x1b' not in result.stdout + result.stderr, f'non-TTY ANSI: {words}')
        return result

    def machine(words):
        result = run(['--json'] + words)
        check(not result.stderr, f'machine stderr: {words}')
        check(len(result.stdout.splitlines()) == 1, f'machine multiline: {words}')
        return json.loads(result.stdout)

    for words in [[], ['server'], ['profile'], ['config'], ['node'], ['profile', 'sources'],
                  ['config', 'statistics'], ['config', 'edit', '--help'], ['server', 'info', '-h']]:
        result = run(words)
        check('Usage: xrm' in result.stdout and 'Examples:' in result.stdout and not result.stderr,
              f'missing local help: {words}')
        if words and words[0] == 'server':
            check('SUBCOMMANDS:\n  profile' not in result.stdout, 'server help leaked root commands')
        if words and words[0] == 'config':
            check('Inventory and resources:' not in result.stdout, 'config help leaked root commands')
    version = run(['--version'])
    check(version.stdout.startswith('xrm ') and not version.stderr, 'offline version failed')
    result = run(['profile', 'create'], None)
    check('--name' in result.stderr and 'Usage: xrm profile create' in result.stderr and not result.stdout,
          'missing required leaf context')
    result = run(['server', 'not-a-command'], None)
    check('Usage: xrm server ' in result.stderr and 'info' in result.stderr, 'unknown child context')
    result = run(['config', 'show', 'Home', '--bad-option'], None)
    check('Usage: xrm config show' in result.stderr, 'unknown option leaf context')
    result = run(['--json', 'profile', 'create'], None)
    check(not result.stdout and json.loads(result.stderr)['error']['code'] == 'invalid_argument', 'JSON error schema')
    check('Usage:' not in result.stderr, 'JSON error polluted with help')
    # Discover the actual tree from its formatter, then check every branch and
    # every published example with an absent daemon (no RPC side effects).
    queue = [[]]
    visited = set()
    examples = set()
    while queue:
        path = queue.pop()
        check(tuple(path) not in visited, f'duplicate help path: {path}')
        visited.add(tuple(path))
        help_result = run(path + ['--help'])
        check('Usage: xrm' in help_result.stdout and not help_result.stderr, f'help failed: {path}')
        check('--help HELP' not in help_result.stdout, f'flag given a metavar: {path}')
        check('avoids conflict' not in help_result.stdout and 'TEXT:{' not in help_result.stdout,
              f'implementation detail in help: {path}')
        check(not re.search(r'\bTEXT(?:\s|:)', help_result.stdout), f'generic metavar: {path}')
        section = ''
        for line in help_result.stdout.splitlines():
            if line.endswith(':') and not line.startswith(' '):
                section = line[:-1]
            child = re.match(r'^  ([a-z][a-z-]*)\s{2,}', line)
            if child and section not in ['OPTIONS', 'GLOBAL OPTIONS (before the command)', 'POSITIONALS', 'Examples']:
                queue.append(path + [child[1]])
            if section == 'Examples' and line.lstrip().startswith('xrm '):
                examples.add(tuple(shlex.split(line.strip())[1:]))
    for example in sorted(examples):
        if example == ('tui',):
            continue
        check('VALUE' not in example, f'unresolved example value: {example}')
        result = run(list(example), 0 if '--help' in example else 3)
        check('Usage:' not in result.stderr, f'example rejected by parser: {example}')
    offline = run(['status'], 3)
    check(offline.stderr.startswith('Error: xrm status') and 'endpoint' in offline.stderr,
          'offline human action/reason')
    check('server info' in offline.stderr and str(socket) in offline.stderr and
          'Diagnostic (original):' not in offline.stderr, 'offline recovery/diagnostic hierarchy')
    verbose_offline = run(['--verbose', 'status'], 3)
    check('Diagnostic (original):' in verbose_offline.stderr, 'verbose original diagnostic unavailable')
    machine_offline = run(['--json', 'status'], 3)
    check(json.loads(machine_offline.stderr)['error']['message'].startswith('连接xrmd失败:'),
          'machine message contract changed')
    check(list(root.iterdir()) == [], 'help/errors created data or attempted daemon setup')

    daemon_log = open(root / 'daemon.log', 'w')
    daemon = subprocess.Popen([str(args.xrmd), '--foreground', '--socket', str(socket),
                               '--database', str(root / 'db' / 'state.sqlite')],
                              env=env, stdout=daemon_log, stderr=daemon_log)
    children = []
    try:
        deadline = time.monotonic() + 5
        while not socket.exists() and time.monotonic() < deadline:
            check(daemon.poll() is None, 'daemon exited at startup')
            time.sleep(.02)
        check(socket.exists(), 'daemon socket not ready')
        check(run(['profile', 'list']).stdout == '', 'non-TTY empty list contract')
        check(machine(['profile', 'list']) == [], 'machine empty list schema')
        created = run(['profile', 'create', '--name', 'Home'])
        check('Created Profile Home [' in created.stdout and 'no core started' in created.stdout, 'create receipt')
        profiles = machine(['profile', 'list'])
        profile_id = profiles[0]['id']
        check(profile_id in run(['profile', 'list']).stdout, 'full ID missing from list')
        check('Saved target:' in run(['profile', 'show', 'Home']).stdout, 'profile details')
        direct = machine(['node', 'add', '--name', 'Direct', '--outbound-json', '{"protocol":"freedom","settings":{}}'])
        node_id = direct['id']
        check(node_id in run(['node', 'list']).stdout, 'node ID missing')
        check('Connection settings: hidden' in run(['node', 'show', node_id]).stdout, 'node secret boundary')
        run(['select', 'node', 'Home', node_id])
        before = machine(['status'])
        saved = run(['config', 'edit', 'Home', 'dns', '--text', '{"dns":{"servers":["localhost"]}}'])
        check('Saved native module dns' in saved.stdout, 'save receipt')
        check(machine(['status'])['state'] == before['state'], 'save changed runtime')
        check(machine(['config', 'show', 'Home', 'stats'])['key'] == 'stats', 'native key intercepted by root command')
        check('saved target: scope is running Profile only' in run(['status']).stdout, 'inactive status scope')
        failed = run(['connect', 'Home'], None)
        check(not failed.stdout and failed.stderr.startswith('Error: xrm connect') and 'Profile Home' in failed.stderr,
              'connect failure context')
        check('core register --help' in failed.stderr and 'core use --help' in failed.stderr and
              'No Xray core is selected' in failed.stderr, 'missing core recovery')
        check('connection.connect' not in failed.stderr, 'RPC method leaked into human error')
        missing = run(['profile', 'show', 'Missing Profile'], 1)
        check('Missing Profile' in missing.stderr and 'profile list' in missing.stderr, 'missing object recovery')
        source_before = machine(['config', 'show', 'Home', 'dns'])['text']
        source_path = next(root.rglob('01_dns.json'))
        original_mode = source_path.parent.stat().st_mode
        source_path.parent.chmod(0o500)
        try:
            rejected = run(['config', 'edit', 'Home', 'dns', '--text', '{"dns":{"servers":["1.1.1.1"]}}'], 3)
        finally:
            source_path.parent.chmod(original_mode)
        check(rejected.stderr.startswith('Error: xrm config edit') and 'Home / dns' in rejected.stderr and
              'config show' in rejected.stderr and 'Diagnostic (original):' not in rejected.stderr,
              'save failure context/recovery/diagnostic hierarchy')
        check(machine(['config', 'show', 'Home', 'dns'])['text'] == source_before, 'failed save changed source')
        check('applied target: no running target' in run(['status']).stdout, 'human null target leak')
        run(['disconnect'])
        run(['settings', 'show'])
        run(['geodata', 'list'])
        run(['group', 'list', 'Home'])

        # The preset supplies all 15 files, including excluded modules. Remove
        # one excluded source only inside this isolated fixture to test absence.
        absent_source = next(root.rglob('13_version.json'))
        check(absent_source.is_relative_to(root), 'source absence fixture escaped isolation')
        absent_source.unlink()
        check(machine(['config', 'show', 'Home', 'version'])['text'] is None, 'absent source fixture')

        # Real terminals at the same narrow/normal/wide widths as the TUI.
        # Pipes retain the original TSV and complete IDs; terminals never emit tabs.
        def terminal_capture(words, width):
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, width, 0, 0))
            terminal = subprocess.Popen(prefix + words, env=env, stdout=slave, stderr=slave)
            os.close(slave)
            terminal_text = b''
            while True:
                try:
                    block = os.read(master, 4096)
                except OSError:
                    break
                if not block:
                    break
                terminal_text += block
            os.close(master)
            check(terminal.wait(timeout=5) == 0, f'PTY failed: {words}')
            output = terminal_text.decode()
            check('\x1b' not in output, f'NO_COLOR ignored: {words}')
            records.append({'argv': words, 'terminal': True, 'width': width, 'code': terminal.returncode,
                            'stdout': output, 'stderr': ''})
            return output

        for width in [80, 120, 180]:
            output = terminal_capture(['profile', 'list'], width)
            check(('Core ID' if width == 80 else 'CORE ID') in output and profile_id in output, 'TTY table/header/ID')
            check('\t' not in output, 'TTY table still relies on tab stops')
            check(all(len(line) <= width for line in output.splitlines()), 'TTY table overflow')
            output = terminal_capture(['node', 'list'], width)
            check(node_id in output and 'PROTOCOL' in output and 'SUPPORT' in output, 'TTY node columns')
            check(all(len(line) <= width for line in output.splitlines()), 'TTY node overflow')
            output = terminal_capture(['config', 'modules', 'Home'], width)
            check('SOURCE FILE' in output and 'RUNTIME FILE' in output, 'module source/runtime distinction')
            check('generated on Connect / Apply' in output and '(not saved)' in output,
                  f'module output location or absent source misrepresented: {output!r}')
            check(all(len(line) <= width for line in output.splitlines()), 'TTY module table overflow')
            if width == 80:
                check('modules/03_outbounds.json' in output and
                      any(line.strip() == '03_outbounds.json' for line in output.splitlines()),
                      'compact Source/Runtime lines lost alignment')
        check('\tHome\t' in run(['profile', 'list']).stdout, 'profile pipe TSV changed')
        output = terminal_capture(['node', 'test-results'], 80)
        check('No node test results' in output and output.strip() != '[]', 'TTY raw RPC array')
        for words in [[], ['profile'], ['profile', 'list'], ['profile', 'create', '--help']]:
            terminal_capture(words, 80)

        if args.xray:
            core = machine(['core', 'register', str(args.xray.resolve()), '--name', 'Fixed Xray'])
            run(['core', 'use', 'Home', core['id']])
            exported = machine(['config', 'export', 'Home', '--output', str(root / 'exported')])
            check(exported['exported'] and (root / 'exported' / '01_dns.json').exists(), 'default saved export')
            check(not (root / 'exported' / 'config.json').exists(), 'export collapsed confdir')
            check(machine(['status'])['state'] == 'stopped', 'default export started core')
            # No inbound listener, TUN, remote subscription or network traffic.
            run(['config', 'edit', 'Home', 'inbounds', '--text', '{"inbounds":[]}'])
            connected = run(['connect', 'Home'])
            check('process running; traffic unverified' in connected.stdout, 'connect overclaim or missing confirmation')
            check('Connected Profile Home [' + profile_id + ']' in connected.stdout, 'connect name and ID')
            check(machine(['status'])['state'] == 'running', 'core not running')
            stopped = run(['disconnect'])
            check('Core is stopped' in stopped.stdout, 'stop receipt')
        else:
            records.append({'scenario': 'successful connect with fixed real Xray', 'status': 'SKIP: --xray not supplied'})

        def watch():
            process = subprocess.Popen(prefix + ['--json', 'stats', '--watch'], env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            children.append(process)
            return process

        def sample(process):
            check(select.select([process.stdout], [], [], 4)[0], 'watch sample not flushed promptly')
            line = process.stdout.readline()
            check(line and isinstance(json.loads(line), dict), 'invalid watch sample')
            return line.decode()

        process = watch()
        first = sample(process)
        second = sample(process)
        process.send_signal(signal.SIGINT)
        out, err = process.communicate(timeout=5)
        check(process.returncode == 0 and not err, 'watch Ctrl-C failed')
        records.append({'scenario': 'watch flush + Ctrl-C', 'code': process.returncode,
                        'stdout': first + second + out.decode(), 'stderr': err.decode()})

        process = watch()
        first = sample(process)
        process.stdout.close()
        check(process.wait(timeout=5) == 0, 'consumer close did not finish normally')
        error = process.stderr.read().decode()
        check(not error, 'consumer close was reported as an error')
        records.append({'scenario': 'watch stdout consumer closed', 'code': process.returncode,
                        'stdout': first, 'stderr': error})

        process = watch()
        first = sample(process)
        daemon.terminate()
        daemon.wait(timeout=5)
        out, err = process.communicate(timeout=6)
        check(process.returncode == 3 and err, f'daemon disconnect exit={process.returncode}, stderr={err!r}')
        check(json.loads(err)['error']['code'] == 'io_error', 'daemon disconnect JSON error lost')
        records.append({'scenario': 'watch daemon disconnected', 'code': process.returncode,
                        'stdout': first + out.decode(), 'stderr': err.decode()})
    finally:
        for process in children:
            if process.poll() is None:
                process.kill()
                process.wait()
        if daemon.poll() is None:
            daemon.terminate()
            daemon.wait(timeout=5)
        daemon_log.close()

if args.evidence:
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    args.evidence.write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n')
print(f'CLI R10/R11 passed: {len(records)} captured scenarios; G5 remains user acceptance')
