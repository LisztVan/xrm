#!/usr/bin/env python3
"""Review remediation: actual isolated tmux events/cells plus authoritative RPC facts.
PNG files, when produced, are ANSI cell re-renderings, not native terminal pixels.
"""
import argparse, json, re, time, subprocess
from pathlib import Path
from terminal_regression import Harness, PAGES, MODULES

class Review(Harness):
    def choose(self, label, capture=None):
        # Help opens only: unchanged server state before selecting an action.
        before = self.rpc('runtime.status')
        self.key('F1'); self.contains('Help & actions')
        assert self.rpc('runtime.status') == before
        self.key('Home')
        for _ in range(60):
            if any(re.search(r'>\s', line) and label in line for line in self.grid().splitlines()):
                if capture: self.capture(capture)
                self.key('Enter')
                return
            self.key('Down')
        raise AssertionError('Help action not found: '+label+'\n'+self.grid())

    def resize(self,w,h):
        self.tmux('resize-window','-t','xrm','-x',str(w),'-y',str(h))
        self.wait(lambda: len(self.grid().splitlines()) == h and
                  (('Terminal too small' in self.grid()) if w < 80 else any('F1 Help' in line for line in self.grid().splitlines()[-4:])), 'resize repaint')
        # tmux updates geometry before the application's SIGWINCH rendering.
        time.sleep(.15)

    def section(self,page,index):
        self.page(page); self.key('Enter')
        # Returning to TopNav retains section; use visible N/total identity.
        total = 6 if page == 'Config' else 3 + len(self.rpc('subscription.list'))
        match = re.search(r'(\d+)/'+str(total),self.grid().splitlines()[1])
        current = int(match.group(1))-1 if match else 0
        for _ in range((index-current)%total): self.key('Tab')
        self.key('Enter')

    def run_editor(self):
        a,b=self.fixture(); before=self.runtime_files(); source=self.source(a,'dns')
        self.page('Profiles'); self.content(); self.profile_cursor(a); self.choose('Allow module Edit')
        self.module('dns'); self.choose('Open JSON')
        self.key('C-Home'); self.type('!'); self.key('C-s'); self.contains('Invalid JSON'); self.contains('line 1')
        self.capture('invalid-json-inline-and-toast-deferred')
        assert self.source(a,'dns')==source
        self.key('F1'); self.capture('invalid-json-help'); self.key('Escape')
        self.key('C-Home','Delete','C-s')
        self.wait(lambda:'Invalid JSON' not in self.grid(),'JSON repaired and saved')
        self.capture('json-saved-input-unobscured')
        self.key('F2'); self.capture('toast-returned-after-json-editing')
        self.wait(lambda:'history in Help' not in self.grid(),'toasts expired after editor',timeout=12)
        self.capture('toast-erased-without-residue')
        assert self.source(a,'dns')==source
        assert self.runtime_files()==before
        self.record('verification',{'status':'PASS','scope':'JSON validation stays inline; Help full validation; invalid source never saved; Toast deferred while typing; explicit source save leaves runtime untouched'})

    def run_screens(self):
        a,b = self.fixture()
        core = self.rpc('profile.get',profile_id=a)['core_id']
        for i in range(16):
            self.rpc('profile.create',name=f'Lab region {i:02} / long descriptive Profile name',core_id=core,preset='custom')
        for i in range(40):
            self.rpc('node.add',name=f'Loopback route {i:02} / long node name for inventory review',outbound={'protocol':'freedom','settings':{}})
        for i in range(12):
            self.rpc('subscription.add',name=f'Local fixture {i:02} / source name',url=f'http://127.0.0.1:9/fixture-{i}',format='auto',auto_refresh=False)
        self.page('Overview'); self.content(); self.key('r')
        self.record('representative_data',{'profiles':18,'nodes':41,'subscriptions':12,'network':'loopback only, automatic refresh disabled'})
        for theme in ['dark','light','default']:
            self.rpc('settings.update',patch={'appearance':{'theme':theme,'color_mode':'truecolor'}})
            self.page('Overview'); self.content(); self.key('r')
            for w,h in [(80,24),(120,40),(180,48)]:
                self.resize(w,h)
                for page in PAGES:
                    self.page(page); self.content()
                    if page=='Modules':
                        self.module('dns')
                    self.capture(f'{theme}-{w}x{h}-{page.lower()}')
                self.page('Profiles'); self.content(); self.key('F1'); self.capture(f'{theme}-{w}-help'); self.key('Escape')
            self.resize(80,24); self.page('Profiles'); self.content(); self.key('R')
            self.capture(theme+'-form'); self.key('End'); self.type(' dirty'); self.key('Escape'); self.capture(theme+'-dialog'); self.choose('Discard')
            self.profile_cursor(a); self.choose('Cancel Use' if 'Use Alpha' in self.grid() else 'Use Profile')
            self.capture(theme+'-toast')
            self.wait(lambda:'history in Help' not in self.grid(),'toast expiry',timeout=9)
            self.capture(theme+'-toast-expired')
        self.record('verification',{'status':'PASS','scope':'seven populated pages x three sizes x all three themes; modal/Toast/Help; current ANSI blank cells'})

    def run_review(self):
        a,b = self.fixture()
        pid = self.rpc('runtime.status')['pid']; runtime = self.runtime_files()
        source = self.rpc('subscription.add',name='Local',url='http://127.0.0.1:9/review?synthetic=1',format='auto')
        self.page('Subscriptions'); self.content(); self.key('r')
        for w,h in [(80,24),(120,40),(180,48)]:
            self.resize(w,h)
            self.page('Modules'); self.key('Enter')
            # Traverse both directions including wrap, retaining each full selected identity.
            for direction in ['Tab','BTab']:
                for i in range(15):
                    self.key(direction)
                    screen = self.grid()
                    assert '/15' in screen.splitlines()[1], screen
                    selected = MODULES[int(re.search(r'(\d+)/15',screen.splitlines()[1]).group(1))-1]
                    assert selected in screen.splitlines()[1],screen
                    self.capture(f'nav-{w}-{direction}-{i:02}-{selected}')
            self.section('Nodes',1); self.capture(f'nodes-local-scope-{w}')
            self.section('Nodes',3); self.capture(f'nodes-local-source-{w}')
        self.resize(80,24)
        for page in PAGES:
            self.page(page); self.content()
            state = self.rpc('runtime.status'); self.key('F1'); self.contains('Help & actions')
            self.capture('help-'+page.lower()); self.key('Escape')
            assert self.rpc('runtime.status') == state
        self.page('Profiles'); self.content(); self.profile_cursor(a)
        self.choose('New Profile','help-new-profile'); self.contains('new Profile')
        self.capture('form-empty'); self.type('Review 中文 é qe?u')
        self.key('Left','BSpace'); self.type('e'); self.key('Home','Delete'); self.type('R')
        self.capture('form-cjk-edited')
        self.choose('Save','help-form-save')
        created = next(p for p in self.rpc('profile.list') if p['name'].startswith('Review'))
        self.record('created_profile',created)
        self.profile_cursor(created['id']); self.choose('View highlighted object')
        self.contains('View Review')
        self.choose('Use Profile'); self.choose('Cancel Use'); self.contains('Next: none')
        assert self.rpc('runtime.status')['pid'] == pid
        self.choose('Profile metadata'); self.capture('profile-metadata-no-edit')
        self.key('End'); self.type(' renamed'); self.choose('Save')
        self.choose('Delete Profile'); self.contains('Delete Profile')
        self.key('F1'); self.capture('help-dialog'); self.key('Escape')
        self.choose('Confirm')
        assert all(p['id'] != created['id'] for p in self.rpc('profile.list'))
        self.profile_cursor(a); self.choose('View highlighted object'); self.choose('Allow module Edit')
        self.profile_cursor(b); self.choose('Use Profile')
        self.capture('view-edit-a-highlight-use-b-running-a')
        assert self.rpc('runtime.status')['pid'] == pid
        # Four rapid notifications, repeat coalescing and modal interruption.
        self.key('u','u','u','u'); self.capture('four-toast-events')
        self.key('R','End'); self.type(' UNSAVED')
        self.capture('toast-hidden-by-form')
        for w,h in [(120,40),(80,24),(60,18),(80,24),(180,48)]:
            self.resize(w,h); self.capture(f'dirty-resize-{w}x{h}')
            if w>=80: assert 'UNSAVED' in self.grid()
        self.resize(80,24); self.key('Escape'); self.capture('unsaved-dialog')
        self.choose('Continue editing'); self.contains('UNSAVED')
        self.key('Escape'); self.choose('Discard')
        self.choose('Notification history'); self.capture('notification-history'); self.key('Escape')
        # A secret widget holds the real URL. Reveal is legal and never mutates it.
        self.page('Subscriptions'); self.content(); self.key('Home')
        self.choose('Edit subscription'); self.key('Tab'); self.capture('url-masked')
        self.key('C-r'); self.contains('127.0.0.1'); self.capture('url-revealed-synthetic')
        self.key('End'); self.type('&q=e?u'); self.choose('Save')
        real = self.rpc('subscription.get',id=source['id'])['subscription']['url']
        assert real == 'http://127.0.0.1:9/review?synthetic=1&q=e?u',real
        self.record('synthetic_url_saved',real)
        # Same process for all provided themes and palettes, including blank cells.
        for theme in ['dark','light','default']:
            self.rpc('settings.update',patch={'appearance':{'theme':theme,'color_mode':'truecolor'}})
            self.page('Overview'); self.content(); self.key('r')
            for w,h in [(80,24),(120,40),(180,48)]:
                self.resize(w,h)
                for page in PAGES:
                    self.page(page); self.content()
                    if page=='Logs': self.key('End')
                    self.capture(f'{theme}-{w}x{h}-{page.lower()}')
                self.page('Profiles'); self.content(); self.key('F1'); self.capture(f'{theme}-{w}-help'); self.key('Escape')
            self.resize(80,24); self.page('Profiles'); self.content(); self.key('R')
            self.capture(theme+'-form'); self.key('End'); self.type(' dirty'); self.key('Escape')
            self.capture(theme+'-dialog'); self.choose('Discard')
        for mode in ['256','none']:
            self.rpc('settings.update',patch={'appearance':{'theme':'dark','color_mode':mode}})
            self.page('Overview'); self.content(); self.key('r'); self.capture('palette-'+mode)
        assert self.runtime_files() == runtime
        assert self.rpc('runtime.status')['pid'] == pid
        # A failed save keeps the form and text. Reconnect does not replay it.
        self.page('Profiles'); self.content(); self.profile_cursor(a); self.key('R','End'); self.type(' OFFLINE-DRAFT')
        self.daemon.terminate(); self.daemon.wait(timeout=5)
        self.key('C-s'); self.contains('OFFLINE-DRAFT'); self.contains('disconnected'); self.capture('failed-save-keeps-draft')
        self.daemon = subprocess.Popen([str(self.build/'xrmd'),'--foreground','--socket',str(self.sock),'--database',str(self.temp/'state.sqlite')],env=self.env,stdout=self.server_log,stderr=subprocess.STDOUT)
        self.wait(lambda:self.sock.exists(),'restarted daemon socket')
        self.contains('xrmd ready'); self.capture('reconnected-draft-not-replayed')
        assert self.rpc('profile.get',profile_id=a)['name']=='Alpha'
        assert self.rpc('runtime.status')['state']=='stopped'
        self.key('Escape'); self.choose('Discard')
        self.record('verification',{'status':'PASS','scope':'review navigation Help profiles URL themes palettes toast resize failed save','runtime':'fake process only; real Xray is separate logs/real-core evidence'})

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--build',type=Path,default=Path('out/dev')); p.add_argument('--output',type=Path,default=Path('docs/evidence/review-remediation/ui')); p.add_argument('--screens-only',action='store_true'); p.add_argument('--editor-only',action='store_true')
    a=p.parse_args(); h=Review(a.build,a.output); h.env.pop('NO_COLOR',None); h.env['COLORTERM']='truecolor'
    try: h.run_editor() if a.editor_only else h.run_screens() if a.screens_only else h.run_review()
    finally: h.close()
