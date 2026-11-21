// Static bionic executables (Android test binaries) need a TLS segment
// aligned to 64 bytes on arm64, or libc aborts at start-up ("executable's TLS
// segment is underaligned"). This file is built with -fno-emulated-tls.
__attribute__((used, aligned(64))) __thread char cvf_tls_align[64];
