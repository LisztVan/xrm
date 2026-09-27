#!/usr/bin/env python3
"""Fixed-core error stages against a disposable daemon; no core is started."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket as socketlib
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--xray', type=Path, required=True)
parser.add_argument('--evidence', type=Path, required=True)
args = parser.parse_args()
build, xray = args.build.resolve(), args.xray.resolve()
args.evidence.mkdir(parents=True, exist_ok=True)
trace = []
with tempfile.TemporaryDirectory(prefix='xrm-uxr-core-errors-') as location:
    root = Path(location)
    env = dict(os.environ)
    for key, child in [('HOME','home'), ('XDG_RUNTIME_DIR','run'),
                       ('XDG_STATE_HOME','state'), ('XDG_DATA_HOME','data'),
                       ('XDG_CONFIG_HOME','config')]:
        path = root / child
        path.mkdir()
        env[key] = str(path)
    socket = root / 'run/xrmd.sock'
    daemon_log = (args.evidence / 'xrmd.log').open('w')
    daemon = subprocess.Popen([str(build / 'xrmd'), '--foreground', '--socket',
                               str(socket), '--database', str(root / 'state/server.sqlite')],
                              env=env, stdout=daemon_log, stderr=subprocess.STDOUT)
    trace.append({'socket': str(socket), 'database': str(root / 'state/server.sqlite'),
                  'core_sha256': hashlib.sha256(xray.read_bytes()).hexdigest(),
                  'xrm_sha256': hashlib.sha256((build/'xrm').read_bytes()).hexdigest(),
                  'xrmd_sha256': hashlib.sha256((build/'xrmd').read_bytes()).hexdigest()})
    try:
        for _ in range(100):
            if socket.exists():
                break
            if daemon.poll() is not None:
                raise RuntimeError('xrmd exited during setup')
            time.sleep(.05)
        assert socket.exists()
        def run(label, command, machine=False):
            argv = [str(build/'xrm'), '--socket', str(socket)]
            if machine:
                argv.append('--json')
            argv += command
            result = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=25)
            trace.append({'label': label, 'command': command, 'json': machine,
                          'code': result.returncode, 'stdout': result.stdout,
                          'stderr': result.stderr})
            return result
        bad_exe = run('non-Xray executable', ['core','register','/bin/true','--name','not-xray'])
        assert bad_exe.returncode and 'working Xray core' in bad_exe.stderr
        unbound = run('create unbound Profile', ['profile','create','--name','Unbound'], True)
        assert unbound.returncode == 0
        missing_core = run('no selected core', ['config','check','Unbound'])
        assert missing_core.returncode and 'Xray core' in missing_core.stderr
        core = run('register fixed Xray', ['core','register',str(xray),'--name','Fixed'], True)
        assert core.returncode == 0, core.stderr
        core_id = json.loads(core.stdout)['id']
        profile = run('create fixed-core Profile', ['profile','create','--name','Invalid inbound','--core',core_id], True)
        assert profile.returncode == 0, profile.stderr
        node = run('add fixed local direct node',
                   ['node','add','--name','Local direct','--outbound-json',
                    '{"protocol":"freedom","settings":{}}'], True)
        assert node.returncode == 0, node.stderr
        selected = run('select saved direct node',
                       ['select','node','Invalid inbound',json.loads(node.stdout)['id']], True)
        assert selected.returncode == 0, selected.stderr
        stopped_runtime = run('stopped runtime is not a missing Profile',
                              ['config','show','Invalid inbound','--source','runtime'])
        assert stopped_runtime.returncode and 'No Xray process is running' in stopped_runtime.stderr
        assert 'Profile' not in stopped_runtime.stderr.split('Reason:', 1)[1].split('Recovery:', 1)[0]
        assert not stopped_runtime.stdout
        still_saved = run('Profile still exists', ['profile','show','Invalid inbound'], True)
        assert still_saved.returncode == 0, still_saved.stderr
        with socketlib.socket() as port_probe:
            port_probe.bind(('127.0.0.1', 0))
            port = port_probe.getsockname()[1]
        native = json.dumps({'inbounds':[{'listen':'127.0.0.1','port':port,
                                          'protocol':'invalid-xrm-test','settings':{}}]})
        saved = run('save core-invalid native inbound as Source',
                    ['config','edit','Invalid inbound','inbounds','--text',native], True)
        assert saved.returncode == 0, saved.stderr
        included = run('include invalid inbound in candidate', ['config','enable','Invalid inbound','inbounds'], True)
        assert included.returncode == 0, included.stderr
        checked = run('fixed Xray rejects candidate', ['config','check','Invalid inbound'])
        verbose = run('raw Xray diagnostic is reachable',
                      ['--verbose','config','check','Invalid inbound'])
        machine = run('machine error code retained', ['config','check','Invalid inbound'], True)
        assert checked.returncode and 'candidate configuration' in checked.stderr, verbose.stderr
        assert '--verbose config check' in checked.stderr
        assert verbose.returncode and 'Diagnostic (original)' in verbose.stderr
        assert machine.returncode and json.loads(machine.stderr)['error']['code'] == 'core_check_failed'
        assert not checked.stdout and not machine.stdout
        trace.append({'result': 'PASS', 'core_was_started': False})
    finally:
        if daemon.poll() is None:
            daemon.terminate()
            daemon.wait(timeout=10)
        daemon_log.close()
        (args.evidence/'trace.json').write_text(json.dumps(trace, ensure_ascii=False, indent=2)+'\n')
print('Fixed-core error stages passed:', args.evidence)
