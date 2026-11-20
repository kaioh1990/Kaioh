# CVE-2026-111497 — Brotli Decoder OOB Write

## Overview
Out-of-bounds write in Google Brotli C decoder (`c/dec/decode.c`).
Invalid block-type switch misreported as "need more input" instead of error.
Decoder continues with stale Huffman state → OOB write on ring buffer.

## iOS Status
- iOS 27.0.1 (Build 24A446): **VULNERABLE**
- iOS 27.1: **VULNERABLE**  
- iOS 27.2.1 (Build 24B156): PATCHED (Nov 9, 2026)

## Attack Vector
HTTPS response with `Content-Encoding: br` containing crafted Brotli stream.
Safari decompresses automatically in WebKit.Networking process.
Zero-click — just load a URL. Works through Lockdown Mode.

## Primitive
Ring buffer OOB write. Decoder state is ~230KB heap allocation.
Attacker-controlled offset from decoder state struct.

## Requirements
- HTTPS (Safari only sends Accept-Encoding: br over HTTPS)
- Need self-signed cert trusted on test device, or mkcert

## Files
- crash_server.py — HTTPS server that serves crafted Brotli response
- gen_brotli_crash.py — Generate malicious Brotli stream
