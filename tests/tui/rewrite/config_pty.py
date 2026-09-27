#!/usr/bin/env python3
"""Real isolated Config PTY workflow. Requires tmux and an explicitly supplied Xray.

python3 tests/tui/rewrite/config_pty.py --build-dir out/dev \
    --xray /absolute/path/to/xray --evidence out/dev/config-pty

No public download: Geodata comes from a temporary loopback-only protobuf fixture.
The script owns and cleans its daemon, tmux server and temporary data directory.
"""
import argparse,json,os,pathlib,re,shlex,subprocess,tempfile,time
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir',type=pathlib.Path,required=True)
parser.add_argument('--xray',type=pathlib.Path,required=True)
parser.add_argument('--evidence',type=pathlib.Path,required=True)
args=parser.parse_args()
BIN=args.build_dir.resolve()/'xrm'
SERVER=args.build_dir.resolve()/'xrmd'
XRAY=args.xray.resolve()
OUT=args.evidence.resolve()
OUT.mkdir(parents=True,exist_ok=True)
steps=[]
with tempfile.TemporaryDirectory(prefix='xrm-config-pty-') as temp:
 root=pathlib.Path(temp); sock=root/'xrmd.sock'; tmux='xrm-cfg-'+str(os.getpid())
 env={**os.environ,'HOME':str(root/'home'),'XDG_CONFIG_HOME':str(root/'config'),'XDG_DATA_HOME':str(root/'data'),'XDG_STATE_HOME':str(root/'state'),'XDG_RUNTIME_DIR':str(root/'run'),'NO_COLOR':'1'}
 log=open(root/'daemon.log','w'); daemon=subprocess.Popen([str(SERVER),'--foreground','--socket',str(sock),'--database',str(root/'db/state.sqlite')],env=env,stdout=log,stderr=log)
 def cli(*args):
  r=subprocess.run([str(BIN),'--socket',str(sock),'--json',*args],env=env,capture_output=True,text=True,timeout=10)
  assert r.returncode==0,(args,r.returncode,r.stdout,r.stderr)
  return json.loads(r.stdout)
 def tx(*args):return subprocess.check_output(['tmux','-L',tmux,*args],text=True)
 def key(*keys):
  steps.append({'keys':list(keys)})
  for item in keys:
   tx('send-keys','-t','config',item);time.sleep(.13)
 def literal(text):
  tx('send-keys','-t','config','-l','--',text);steps.append({'text':text});time.sleep(.13)
 def has_text(text, fragment):
  # Toasts wrap inside their bounded overlay; match the same complete phrase
  # across terminal rows without treating box-drawing characters as data.
  plain = re.sub(r'[\u2500-\u257f]', ' ', text)
  return re.search(r'\s+'.join(re.escape(word) for word in fragment.split()), plain) is not None
 def capture(name,contains=None):
  text=tx('capture-pane','-p','-t','config');(OUT/(name+'.txt')).write_text(text)
  steps.append({'screen':name,'text':text})
  if contains: assert has_text(text, contains),(name,contains,text)
  return text
 def until(name,fragment):
  for i in range(70):
   text=tx('capture-pane','-p','-t','config')
   if has_text(text, fragment):return capture(name,fragment)
   time.sleep(.1)
  raise AssertionError((name,fragment,text))
 try:
  for i in range(100):
   if sock.exists():break
   time.sleep(.03)
  profile=cli('profile','create','--name','Home');original=cli('config','show','Home','inbounds')
  tx('-f','/dev/null','new-session','-d','-s','config','-x','120','-y','40',
     *['env',*[k+'='+env[k] for k in ['HOME','XDG_CONFIG_HOME','XDG_DATA_HOME','XDG_STATE_HOME','XDG_RUNTIME_DIR','NO_COLOR']],str(BIN),'--socket',str(sock),'tui'])
  time.sleep(1);capture('01-overview','Overview')
  key('Tab','Tab','Tab','Tab','Tab','Tab','Enter','Enter','a');capture('02-core-form','Executable path')
  literal('Fixture Xray');key('Tab');literal('/missing/xray');key('C-s');until('03-core-invalid','/missing/xray')
  assert len(cli('core','list'))==0
  key('Home',*(['Delete']*13));literal(str(XRAY));key('C-s');until('04-core-saved','Registered Fixture Xray')
  cores=cli('core','list');assert len(cores)==1,cores
  key('u');until('04b-core-selected','Use core: completed')
  assert cli('profile','show','Home')['core_id']==cores[0]['id']
  # Core Content -> Secondary Tests -> Content, four fields in one form.
  key('Escape','Tab','Tab','Enter','e');capture('05-tests-form','Concurrent tests')
  literal('http://127.0.0.1/fixture?quep');key('Tab','Home',*(['Delete']*4));literal('4500')
  key('Tab','Home','Delete');literal('9');key('C-s');capture('06-tests-invalid','9')
  assert cli('settings','show')['tests']['concurrency']==3
  key('Home','Delete');literal('2');key('C-s');until('07-tests-saved','Node test defaults saved')
  settings=cli('settings','show');assert settings['tests']['timeout_ms']==4500 and settings['tests']['concurrency']==2,settings
  # Entering '?' above was text and did not open Help or trigger a background action.
  assert settings['tests']['default_url']=='http://127.0.0.1/fixture?quep',settings
  key('Escape','Tab','Tab','Enter','e');capture('08-editor-form','Editor arguments')
  literal('/usr/bin/vi');key('Enter');literal('-n');key('C-s');until('09-editor-saved','External editor saved')
  assert cli('settings','show')['editor']['argv']==['/usr/bin/vi','-n']
  # Go to Appearance, use a finite selector.
  key('Escape','BTab','Enter','e');capture('10-appearance-form','Color mode')
  # Save uses the framework's deferred button callback, unlike Ctrl-S. Verify
  # actual changes, all palettes, and reopening the form after each close.
  for index,(theme,selection) in enumerate([
      ('light',('End',)),('default',('Home',)),('dark',('Home','Down'))]):
   if index: key('e')
   key(*selection,'Space',*(['Tab']*6),'Enter')
   until('11-appearance-saved-'+theme,'XRM appearance saved')
   assert cli('settings','show')['appearance']['theme']==theme
   capture('11-appearance-closed-'+theme,'Content')
  # Resource lifecycle uses a temporary local file; no public download.
  fixture = root/'local-geoip.dat'; fixture.write_bytes(b'not protobuf')
  key('Escape','BTab','BTab','Enter','a');capture('12-geodata-form','Runtime filename')
  literal('Geo Fixture');key('Tab','Tab','Tab','Down','Space','Tab');literal(str(fixture))
  key('Tab','Tab','Home','Delete');literal('60');key('Tab','Down','Space','C-s')
  until('13-geodata-created','Geodata Geo Fixture saved')
  resources=cli('geodata','list');assert len(resources)==1,resources
  rid=resources[0]['id'];assert resources[0]['update_interval_seconds']==60 and not resources[0]['auto_update'],resources
  assert resources[0]['validator_core_id']==cores[0]['id'],resources
  key('e','End');literal(' edited');key('C-s');until('14-geodata-edited','Geodata Geo Fixture edited saved')
  assert cli('geodata','list')[0]['name']=='Geo Fixture edited'
  key('b','Space','C-s');capture('15-geodata-bind-rejected','Update and validate')
  assert cli('profile','show','Home')['geodata_resource_ids']=={}
  key('Escape','Right','Enter')
  # Valid loopback-only protobuf fixture, same encoding as geodata_application_real_tests.cpp.
  def field(number, payload):return bytes([(number<<3)|2,len(payload)])+payload
  fixture.write_bytes(field(1,field(1,b'XRM-TEST')+field(2,field(1,b'\x7f\x00\x00\x00')+b'\x10\x08')))
  key('u')
  for i in range(100):
   resources=cli('geodata','list')
   if resources[0]['current'] is not None:break
   time.sleep(.1)
  assert resources[0]['current'] is not None,resources
  until('16-geodata-validated','Update resource: completed')
  key('b','Space','C-s');until('17-geodata-bound','Geodata bindings saved for Home')
  assert cli('profile','show','Home')['geodata_resource_ids']=={'geoip.dat':rid}
  key('D','BTab','Enter');time.sleep(.3);capture('18-bound-remove-rejected')
  assert len(cli('geodata','list'))==1
  key('b','Space','C-s');until('19-geodata-unbound','Geodata bindings saved for Home')
  assert cli('profile','show','Home')['geodata_resource_ids']=={}
  key('D','BTab','Enter');time.sleep(.3);capture('20-geodata-removed')
  assert cli('geodata','list')==[]
  # Local entry only navigates to inbounds; opening cannot write or enable it.
  key('Escape','Escape','BTab','BTab','BTab','BTab','BTab','BTab','Enter','i')
  capture('21-local-entry-readonly','Source read-only')
  assert cli('config','show','Home','inbounds')==original
  assert cli('status')['state']=='stopped'
 finally:
  (OUT/'steps.json').write_text(json.dumps(steps,ensure_ascii=False,indent=2))
  subprocess.run(['tmux','-L',tmux,'kill-server'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
  if daemon.poll() is None:daemon.terminate();daemon.wait(timeout=5)
  log.close()
print('Config PTY form/edit/failure/readonly flows passed:',OUT)
