#!/usr/bin/env python3
"""Drop one real daemon receipt after commit. Never fake the write or its result."""
import argparse, json, socket, socketserver, threading
from pathlib import Path
from review_remediation import Review

class ReceiptDrop(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            request=json.loads(line)
            with socket.socket(socket.AF_UNIX) as upstream:
                upstream.settimeout(6); upstream.connect(str(self.server.upstream))
                upstream.sendall(line); reply=upstream.makefile('rb').readline()
            if request['method']=='module.save':
                self.server.writes.append({'request':request,'actual_reply':json.loads(reply)})
                if self.server.drop_next:
                    self.server.drop_next=False
                    return  # Server has committed; client never receives this success.
            if request['method']=='operation.get':
                self.server.operation_reads.append(request['params']['operation_id'])
                if self.server.drop_operation:
                    self.server.drop_operation=False
                    return
            self.wfile.write(reply); self.wfile.flush()
class Proxy(socketserver.ThreadingUnixStreamServer):
    daemon_threads=True

p=argparse.ArgumentParser();p.add_argument('--build',type=Path,default=Path('out/dev'));p.add_argument('--output',type=Path,default=Path('docs/evidence/review-remediation/unconfirmed'))
a=p.parse_args();h=Review(a.build,a.output);proxy=None
try:
    profile=h.rpc('profile.create',name='Unconfirmed save',preset='custom')['id']
    original=h.rpc('module.get',profile_id=profile,module='log')['text']
    actual_socket=h.sock; proxy_path=h.temp/'receipt-proxy.sock'
    proxy=Proxy(str(proxy_path),ReceiptDrop);proxy.upstream=actual_socket;proxy.drop_next=True;proxy.writes=[];proxy.drop_operation=False;proxy.operation_reads=[]
    threading.Thread(target=proxy.serve_forever,daemon=True).start()
    h.sock=proxy_path;h.start_tui(profile);h.page('Profiles');h.content();h.choose('Allow module Edit')
    h.module('log');h.choose('Open JSON');h.key('C-Home');h.type(' ');h.key('C-s')
    h.contains('Confirm the result');h.contains('UNSAVED');h.capture('committed-but-receipt-lost')
    h.wait(lambda:len(proxy.writes)==1,'one forwarded write')
    assert proxy.writes[0]['actual_reply']['ok'],proxy.writes
    saved=h.rpc('module.get',profile_id=profile,module='log')['text']
    assert saved != original and saved.startswith(' '),(saved,original)
    h.contains('xrmd ready');h.capture('reconnected-unsaved-draft-kept')
    assert len(proxy.writes)==1,'UI replayed a write on reconnect'
    assert h.rpc('runtime.status')['state']=='stopped'
    assert h.runtime_files()=={}
    h.record('actual_commit_and_lost_receipt',proxy.writes)
    # A known operation ID is retained after its query receipt is lost.
    h.key('Escape'); h.choose('Discard')
    h.rpc('subscription.add',name='Loopback failed update',url='http://127.0.0.1:9/no-server',format='auto',auto_refresh=False)
    h.page('Subscriptions');h.content();h.key('r');proxy.drop_operation=True
    h.choose('Update subscription')
    h.wait(lambda:len(proxy.operation_reads)>=2,'operation result re-queried after lost response')
    assert proxy.operation_reads[0]==proxy.operation_reads[1],proxy.operation_reads
    h.capture('known-operation-requeried-without-replay')
    h.record('operation_queries',proxy.operation_reads)
    h.record('verification',{'status':'PASS','server':'real isolated xrmd','fault':'forward real module.save; read successful actual reply; close socket without forwarding receipt','writes_observed':len(proxy.writes),'source_changed':True,'TUI_draft_retained':True,'automatic_retry':False,'runtime':'stopped; no generated confdir'})
finally:
    h.close()
    if proxy:proxy.shutdown();proxy.server_close()
