#!/usr/bin/env python3
"""Run the real TUI against disposable xrmd data and a loopback subscription."""
import argparse, contextlib, http.server, json, os, pathlib, signal, subprocess, tempfile, threading, time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', default='out/dev', help='Build containing xrm and xrmd')
parser.add_argument('--core', help='Optional explicit Xray executable to register; never started automatically')
parser.add_argument('--record', help='Write lab paths and PIDs here for terminal inspection')
args = parser.parse_args()
build = pathlib.Path(args.build).resolve()
with tempfile.TemporaryDirectory(prefix='xrm-component-lab-') as directory:
    root = pathlib.Path(directory)
    env = os.environ.copy()
    for key, child in [('HOME','home'), ('XDG_RUNTIME_DIR','run'), ('XDG_STATE_HOME','state'), ('XDG_DATA_HOME','data'), ('XDG_CONFIG_HOME','config')]:
        path = root / child; path.mkdir(); env[key] = str(path)
    sock = root / 'run/xrmd.sock'
    class Fixture(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = b'vless://00000000-0000-0000-0000-000000000001@127.0.0.1:19090?encryption=none&security=none&type=tcp#Loopback%20fixture\n'
            self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
        def log_message(self,*_): pass
    http = http.server.ThreadingHTTPServer(('127.0.0.1',0),Fixture)
    thread = threading.Thread(target=http.serve_forever,daemon=True); thread.start()
    log = (root/'daemon.log').open('w')
    daemon = subprocess.Popen([str(build/'xrmd'),'--foreground','--socket',str(sock),'--database',str(root/'state/server.sqlite')],env=env,stdout=log,stderr=log)
    try:
        for _ in range(100):
            if sock.exists(): break
            if daemon.poll() is not None: raise RuntimeError((root/'daemon.log').read_text())
            time.sleep(.05)
        def cli(*argv):
            result = subprocess.run([str(build/'xrm'),'--socket',str(sock),'--json',*argv],env=env,capture_output=True,text=True)
            if result.returncode: raise RuntimeError(result.stderr)
            return json.loads(result.stdout)
        core = cli('core','register',str(pathlib.Path(args.core).resolve()),'--name','Lab Xray') if args.core else None
        profile_args = ['profile','create','--name','Lab Home']
        if core: profile_args += ['--core',core['id']]
        profile = cli(*profile_args)
        node = cli('node','add','--name','Loopback direct','--outbound-json','{"protocol":"freedom","settings":{}}')
        cli('select','node',profile['id'],node['id'])
        subscription_url = f'http://127.0.0.1:{http.server_address[1]}/nodes'
        source = cli('sub','add','--name','Local fixture','--url',subscription_url)
        cli('profile','sources','set',profile['id'],'--source',source['id'])
        record = {'directory':str(root),'socket':str(sock),'daemon_pid':daemon.pid,
                  'database':str(root/'state/server.sqlite'),'profile':profile['id'],
                  'source':source['id'],'subscription_url':subscription_url,
                  'loopback_node':'Loopback direct','core':str(pathlib.Path(args.core).resolve()) if args.core else None,
                  'build':str(build)}
        if args.record: pathlib.Path(args.record).write_text(json.dumps(record,indent=2)+'\n')
        print('Disposable lab:', root, flush=True)
        print('No core is started automatically. Exit TUI to clean this lab.',flush=True)
        subprocess.run([str(build/'xrm'),'--socket',str(sock),'tui'],env=env,check=True)
    finally:
        if daemon.poll() is None:
            daemon.terminate()
            try: daemon.wait(timeout=10)
            except subprocess.TimeoutExpired: daemon.kill(); daemon.wait()
        http.shutdown(); http.server_close(); log.close()
