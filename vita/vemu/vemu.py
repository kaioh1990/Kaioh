#!/usr/bin/env python3
"""Tiny PS Vita user-mode emulator for the CvF Online eboot (pre vita-elf-create ELF).

Runs the ARM code in Unicorn and implements the Sce* imports it needs in
Python. vitaGL's gl*/vgl* entry points are replaced by stubs (no GPU).
Usage: vemu.py <elf> <stubs.txt from vita-elf-create -v> [--max-sec N]
"""
import os, sys, struct, time, bisect, subprocess, argparse, stat as pystat
sys.path.insert(0, os.environ.get('UCPY', ''))
from unicorn import *
from unicorn.arm_const import *

ap = argparse.ArgumentParser()
ap.add_argument('elf'); ap.add_argument('stubs')
ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vux0'))
ap.add_argument('--max-sec', type=float, default=600)
ap.add_argument('--trace-stubs', action='store_true')
ap.add_argument('--frames', type=int, default=0, help='stop after N vglSwapBuffers')
ap.add_argument('--velf', default='')
ap.add_argument('--base0', type=lambda x:int(x,0), default=0x81075000)
ap.add_argument('--base1', type=lambda x:int(x,0), default=0)
ap.add_argument('--trace-sym', default='')
ap.add_argument('--press-after', default='')
ap.add_argument('--frames-after', type=int, default=0)
ap.add_argument('--press', default='', help='frame:buttonmask,... controller presses')
args = ap.parse_args()
ROOT = os.path.abspath(args.root)
NM = os.environ.get('NM', 'arm-vita-eabi-nm')

# ---------------------------------------------------------------- ELF load
elf = open(args.elf, 'rb').read()
e_phoff, = struct.unpack_from('<I', elf, 28); phnum, = struct.unpack_from('<H', elf, 44)
e_entry, = struct.unpack_from('<I', elf, 24)
mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
try:
    mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
except Exception:
    pass
PAGE = 0x1000
def pgup(x): return (x + PAGE - 1) & ~(PAGE - 1)
segs = []
LINKSEG = []
for i in range(phnum):
    p_type, off, va, pa, fsz, msz, flg, al = struct.unpack_from('<8I', elf, e_phoff + 32 * i)
    if p_type != 1: continue
    LINKSEG.append((va, msz))
DELTA = [0, 0]
if not args.velf:
    for i in range(phnum):
        p_type, off, va, pa, fsz, msz, flg, al = struct.unpack_from('<8I', elf, e_phoff + 32 * i)
        if p_type != 1: continue
        base = va & ~(PAGE - 1)
        mu.mem_map(base, pgup(va + msz) - base)
        mu.mem_write(va, elf[off:off + fsz])
        segs.append((va, msz))
else:
    vf = open(args.velf, 'rb').read()
    vph = struct.unpack_from('<I', vf, 28)[0]; vn = struct.unpack_from('<H', vf, 44)[0]
    vsegs = [struct.unpack_from('<8I', vf, vph + 32 * i) for i in range(vn)]
    loads = [p for p in vsegs if p[0] == 1]
    bases = [args.base0, args.base1 or (args.base0 + (loads[1][2] - loads[0][2]))]
    for (p, b) in zip(loads, bases):
        mu.mem_map(b, pgup(p[5]))
        mu.mem_write(b, vf[p[1]:p[1] + p[4]])
        segs.append((b, p[5]))
    DELTA = [bases[0] - loads[0][2], bases[1] - loads[1][2]]
    rel = [p for p in vsegs if p[0] == 0x60000000][0]
    i = rel[1]; end = rel[1] + rel[4]; nrel = 0
    while i < end:
        w0 = struct.unpack_from('<I', vf, i)[0]
        short = w0 & 0xF; symseg = (w0 >> 4) & 0xF; code = (w0 >> 8) & 0xFF; datseg = (w0 >> 16) & 0xF
        if short:
            w1 = struct.unpack_from('<I', vf, i + 4)[0]; o = (w0 >> 20) | ((w1 & 0x3FF) << 12); a_ = w1 >> 10; i += 8
        else:
            a_, o = struct.unpack_from('<II', vf, i + 4); i += 12
        S_ = bases[symseg]; P = bases[datseg] + o; T = (S_ + a_) & 0xffffffff
        if code in (2, 38):
            mu.mem_write(P, struct.pack('<I', T))
        elif code in (43, 44):
            ins = struct.unpack('<I', mu.mem_read(P, 4))[0]; v = T & 0xffff if code == 43 else T >> 16
            ins = (ins & 0xFFF0F000) | ((v & 0xF000) << 4) | (v & 0xFFF); mu.mem_write(P, struct.pack('<I', ins))
        elif code == 29:
            # Real Vita loader behaviour: JUMP24 is re-encoded as B, so a
            # conditional BL loses its link bit.
            ins = struct.unpack('<I', mu.mem_read(P, 4))[0]
            if (ins >> 28) != 0xE and ((ins >> 24) & 0xF) == 0xB:
                mu.mem_write(P, struct.pack('<I', ins & ~(1 << 24)))
        elif code in (47, 48):
            h1, h2 = struct.unpack('<HH', mu.mem_read(P, 4)); v = T & 0xffff if code == 47 else T >> 16
            h1 = (h1 & 0xFBF0) | ((v >> 12) & 0xF) | (((v >> 11) & 1) << 10)
            h2 = (h2 & 0x8F00) | (((v >> 8) & 7) << 12) | (v & 0xFF)
            mu.mem_write(P, struct.pack('<HH', h1, h2))
        nrel += 1
    print('[vemu] relocated %d entries, bases %x %x' % (nrel, bases[0], bases[1]))
def L2R(a):
    for (lo, sz), d in zip(LINKSEG, DELTA):
        if lo <= (a & ~1) < lo + sz: return (a + d) & 0xffffffff
    return a
def R2L(a):
    for (lo, sz), d in zip(LINKSEG, DELTA):
        if lo + d <= (a & ~1) < lo + d + sz: return (a - d) & 0xffffffff
    return a
TEXT_LO, TEXT_HI = segs[0][0], segs[0][0] + segs[0][1]

# symbols
syms = []
ALLSYM = {}
for line in subprocess.run([NM, '-n', args.elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3: ALLSYM.setdefault(p[2], int(p[0], 16))
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
symaddr = [a for a, _ in syms]
byname = {}
for a, n in syms: byname.setdefault(n, a)
def symbolize(a):
    a = R2L(a)
    i = bisect.bisect_right(symaddr, a & ~1) - 1
    if i < 0: return hex(a)
    return '%s+0x%x' % (syms[i][1][:90], (a & ~1) - syms[i][0])

# ---------------------------------------------------------------- memory helpers
def r32(a): return struct.unpack('<I', mu.mem_read(a, 4))[0]
def w32(a, v): mu.mem_write(a, struct.pack('<I', v & 0xffffffff))
def w64(a, v): mu.mem_write(a, struct.pack('<Q', v & 0xffffffffffffffff))
def cstr(a, mx=4096):
    out = b''
    while len(out) < mx:
        ch = bytes(mu.mem_read(a + len(out), 64))
        z = ch.find(b'\0')
        if z >= 0: return (out + ch[:z]).decode('utf-8', 'replace')
        out += ch
    return out.decode('utf-8', 'replace')
def reg(n): return mu.reg_read(UC_ARM_REG_R0 + n) if n < 13 else None
def arg(i):
    if i < 4: return mu.reg_read([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3][i])
    return r32(mu.reg_read(UC_ARM_REG_SP) + 4 * (i - 4))

# bump region for emulator-owned memory (stacks, mem blocks, strings)
NEXT_MAP = [int(os.environ.get('VEMU_MAPBASE', '0x90000000'), 0)]
GARBAGE = os.environ.get('VEMU_GARBAGE', '1') == '1'
def vmap(size, align=PAGE, garbage=False):
    a = (NEXT_MAP[0] + align - 1) & ~(align - 1)
    size = pgup(size)
    mu.mem_map(a, size)
    if garbage and GARBAGE:
        pat = (b'\xa5\x5a\x3c\xc3' * (1 << 18))
        for o in range(0, size, len(pat)):
            mu.mem_write(a + o, pat[:min(len(pat), size - o)])
    NEXT_MAP[0] = a + size + PAGE  # guard page
    return a

# ---------------------------------------------------------------- CPU setup
STACK_SIZE = 1 << 20
stack = vmap(STACK_SIZE, garbage=True)
mu.reg_write(UC_ARM_REG_SP, stack + STACK_SIZE - 16)
try:
    mu.reg_write(UC_ARM_REG_C1_C0_2, 0x00F00000)
except UcError:
    pass
mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
mu.reg_write(UC_ARM_REG_FPSCR, 0x03000000)  # Vita default (flush-to-zero)
MAGIC = vmap(PAGE)          # return addresses we catch: MAGIC = exit main, MAGIC+4 = fiber done
mu.mem_write(MAGIC, b'\xfe\xff\xff\xea' * 8)  # b . (never executed: hooked)
misc = vmap(1 << 20)        # strings etc
MISC_PTR = [misc]
def alloc_misc(data):
    a = MISC_PTR[0]; mu.mem_write(a, data); MISC_PTR[0] = (a + len(data) + 15) & ~15; return a

# ---------------------------------------------------------------- stub table
stubs = {}
for line in open(args.stubs):
    line = line.strip()
    if line.startswith('stub=0x'):
        a = int(line.split()[0][5:], 16)
        name = line.rsplit('(', 1)[1].rstrip(')')
        stubs[L2R(a)] = name

class Stop(Exception): pass
handlers = {}
def H(*names):
    def d(f):
        for n in names: handlers[n] = f
        return f
    return d
RET = object()
calls = {}

CLOB_CALL = os.environ.get('VEMU_CLOBCALL', '1') == '1'
def clobber_call(keep_r1=False):
    if not CLOB_CALL: return
    for r in ([UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12] + ([] if keep_r1 else [UC_ARM_REG_R1])):
        mu.reg_write(r, 0xdead0000 | (r & 0xff))
    for i in list(range(0, 8)) + list(range(16, 32)):
        mu.reg_write(UC_ARM_REG_D0 + i, 0x7ff4dead00000000 | i)
def do_return(val=0, val64=None):
    clobber_call(keep_r1=val64 is not None)
    if val64 is not None:
        mu.reg_write(UC_ARM_REG_R0, val64 & 0xffffffff); mu.reg_write(UC_ARM_REG_R1, (val64 >> 32) & 0xffffffff)
    elif val is not None:
        mu.reg_write(UC_ARM_REG_R0, val & 0xffffffff)
    mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

LOG = []
def say(*a):
    s = ' '.join(str(x) for x in a)
    print(s, flush=True)

# ---------------------------------------------------------------- kernel: memory
memblocks = {}
NEXT_UID = [0x100]
def new_uid():
    NEXT_UID[0] += 1; return NEXT_UID[0]
MEM_BUDGET = [0]
@H('sceKernelAllocMemBlock')
def _(name, typ, size, opt):
    n = cstr(name)
    if MEM_BUDGET[0] + size > int(os.environ.get('VEMU_BUDGET_MB', '360')) * 1024 * 1024:
        return 0x80020002 | 0x80000000
    a = vmap(size, 0x100000 if size >= 0x100000 else PAGE, garbage=True)
    uid = new_uid(); memblocks[uid] = (a, size, n); MEM_BUDGET[0] += size
    say('[mem] AllocMemBlock %s 0x%x -> 0x%x' % (n, size, a))
    return uid
@H('sceKernelGetMemBlockBase')
def _(uid, pbase, *_):
    if uid not in memblocks: return 0x80020001
    w32(pbase, memblocks[uid][0]); return 0
@H('sceKernelFreeMemBlock')
def _(uid, *_):
    if uid in memblocks:
        a, s, n = memblocks.pop(uid); MEM_BUDGET[0] -= s
        mu.mem_unmap(a, pgup(s))
    return 0
@H('sceKernelGetFreeMemorySize')
def _(info, *_):
    # struct {size, size_user, size_cdram, size_phycont}
    tot = int(os.environ.get('VEMU_BUDGET_MB', '360')) << 20
    w32(info + 4, tot - MEM_BUDGET[0]); w32(info + 8, 100 << 20); w32(info + 12, 20 << 20); return 0
@H('sceKernelGetMemBlockInfoByAddr')
def _(addr, info, *_): return 0x80020001
@H('sceAppMgrGetBudgetInfo')
def _(*a): return 0

# mspace allocator (newlib malloc sits on this)
class Mspace:
    def __init__(s, base, size): s.base = base; s.end = base + size; s.ptr = (base + 64 + 15) & ~15; s.free = {}; s.sizes = {}
mspaces = {}
def ms_alloc(m, size, align=16):
    size = max(16, (size + 15) & ~15)
    if align <= 16:
        lst = m.free.get(size)
        if lst:
            p = lst.pop(); m.sizes[p] = size; return p
    p = (m.ptr + align - 1) & ~(align - 1)
    if p + size > m.end: return 0
    m.ptr = p + size; m.sizes[p] = size; return p
@H('sceClibMspaceCreate')
def _(base, size, *_):
    mspaces[base] = Mspace(base, size); say('[mem] mspace 0x%x size 0x%x' % (base, size)); return base
@H('sceClibMspaceDestroy')
def _(*a): return 0
@H('sceClibMspaceMalloc')
def _(m, size, *_): return ms_alloc(mspaces[m], size)
@H('sceClibMspaceCalloc')
def _(m, n, size, *_):
    p = ms_alloc(mspaces[m], n * size)
    if p: mu.mem_write(p, b'\0' * (max(16, (n * size + 15) & ~15)))
    return p
@H('sceClibMspaceMemalign')
def _(m, al, size, *_): return ms_alloc(mspaces[m], size, max(al, 16))
@H('sceClibMspaceFree')
def _(m, p, *_):
    if p:
        ms = mspaces[m]; s = ms.sizes.pop(p, None)
        if s: ms.free.setdefault(s, []).append(p)
    return 0
@H('sceClibMspaceRealloc')
def _(m, p, size, *_):
    ms = mspaces[m]
    if p == 0: return ms_alloc(ms, size)
    old = ms.sizes.get(p, 0)
    if size <= old: return p
    q = ms_alloc(ms, size)
    if q:
        mu.mem_write(q, bytes(mu.mem_read(p, old)))
        ms.sizes.pop(p, None); ms.free.setdefault(old, []).append(p)
    return q
@H('sceClibMspaceMallocUsableSize')
def _(p, *_):
    for ms in mspaces.values():
        if p in ms.sizes: return ms.sizes[p]
    return 0
@H('sceClibMspaceMallocStats')
def _(m, st, *_):
    ms = mspaces[m]; w32(st, ms.end - ms.base); w32(st + 4, ms.end - ms.ptr); w32(st + 8, ms.ptr - ms.base); w32(st + 12, ms.ptr - ms.base); return 0

# clib
OVL = [0]
@H('sceClibMemmove')
def _(d, s, n, *_):
    if n: mu.mem_write(d, bytes(mu.mem_read(s, n)))
    return d
@H('sceClibMemcpy', 'sceDmacMemcpy')
def _(d, s, n, *_):
    if n and d != s and (d < s + n and s < d + n):
        OVL[0] += 1
        if OVL[0] <= 20: say('[warn] overlapping memcpy d=%x s=%x n=%d lr=%s' % (d, s, n, symbolize(mu.reg_read(UC_ARM_REG_LR))))
    if n: mu.mem_write(d, bytes(mu.mem_read(s, n)))
    return d
@H('sceClibMemset')
def _(d, c, n, *_):
    if n: mu.mem_write(d, bytes([c & 0xff]) * n)
    return d
@H('sceClibMemcmp')
def _(a, b, n, *_):
    x = bytes(mu.mem_read(a, n)); y = bytes(mu.mem_read(b, n))
    return 0 if x == y else (-1 if x < y else 1)
@H('sceClibStrcmp')
def _(a, b, *_):
    x = cstr(a); y = cstr(b); return 0 if x == y else (-1 if x < y else 1)
@H('sceClibPrintf')
def _(f, *_):
    say('[printf] ' + cstr(f).rstrip()); return 0

# ---------------------------------------------------------------- threads / sync (single threaded)
threads = {}
@H('sceKernelGetThreadId')
def _(*a): return 0x40010003
@H('sceKernelGetProcessId')
def _(*a): return 0x10005
tls = {}
TLS_AREA = vmap(PAGE)
@H('sceKernelGetTLSAddr', 'sceKernelGetThreadTLSAddr')
def _(*a):
    key = a[0] if handlers_cur[0] == 'sceKernelGetTLSAddr' else a[1]
    return TLS_AREA + (key & 0xff) * 4
handlers_cur = ['']
@H('sceKernelCreateThread')
def _(name, entry, prio, stk, attr, aff, opt):
    uid = new_uid(); threads[uid] = (cstr(name), entry); say('[thread] create %s entry %s' % (cstr(name), symbolize(entry))); return uid
@H('sceKernelStartThread')
def _(uid, argsz, argp, *_):
    say('[thread] start %s (not run)' % threads.get(uid, ('?',))[0]); return 0
@H('sceKernelWaitThreadEnd')
def _(uid, pstat, *_):
    if pstat: w32(pstat, 0)
    return 0
@H('sceKernelDeleteThread', 'sceKernelChangeThreadPriority', 'sceKernelExitThread', 'sceKernelExitDeleteThread')
def _(*a): return 0
@H('sceKernelGetThreadInfo')
def _(uid, info, *_): return 0
@H('sceKernelCreateLwMutex', 'sceKernelDeleteLwMutex', 'sceKernelLockLwMutex', 'sceKernelUnlockLwMutex', 'sceKernelTryLockLwMutex')
def _(*a): return 0
@H('sceKernelCreateMutex', 'sceKernelCreateSema', 'sceKernelCreateMsgPipe')
def _(*a): return new_uid()
@H('sceKernelDeleteMutex', 'sceKernelLockMutex', 'sceKernelUnlockMutex', 'sceKernelDeleteSema', 'sceKernelSignalSema',
   'sceKernelWaitSema', 'sceKernelPollSema', 'sceKernelDeleteMsgPipe', 'sceKernelSendMsgPipe', 'sceKernelReceiveMsgPipe')
def _(*a): return 0
@H('sceKernelGetSemaInfo')
def _(*a): return 0
VTIME = [0]  # microseconds of virtual time added by delays
@H('sceKernelDelayThread', 'sceKernelDelayThreadCB')
def _(us, *_): VTIME[0] += us; return 0
T0 = time.time()
def now_us(): return int((time.time() - T0) * 1e6) + VTIME[0]
@H('sceKernelGetProcessTimeWide')
def _(*a): do_return(val64=now_us()); return RET
@H('sceKernelGetProcessTimeLow')
def _(*a): return now_us() & 0xffffffff
@H('sceKernelLibcGettimeofday')
def _(tv, tz, *_):
    t = time.time() + VTIME[0] / 1e6
    if tv: w32(tv, int(t)); w32(tv + 4, int((t % 1) * 1e6))
    return 0
@H('sceKernelPowerTick')
def _(*a): return 0
@H('sceKernelGetRandomNumber')
def _(out, n, *_): mu.mem_write(out, os.urandom(n)); return 0
@H('sceKernelExitProcess')
def _(code, *_):
    say('[exit] sceKernelExitProcess(%d)' % code); raise Stop()
@H('sceKernelLoadStartModule')
def _(path, *a): say('[mod] LoadStartModule %s' % cstr(path)); return 0x80010002
@H('sceKernelStopUnloadModule', 'sceKernelGetModuleInfo', 'taiGetModuleInfo', 'taiHookRelease', 'taiInjectRelease',
   'taiInjectDataForUser', 'taiHookFunctionOffsetForUser')
def _(*a): return 0x80010002
@H('sceSysmoduleLoadModule')
def _(*a): return 0

# rtc
@H('sceRtcGetCurrentTick')
def _(p, *_): w64(p, int((time.time() + 62135596800) * 1e6)); return 0
@H('sceRtcGetTickResolution')
def _(*a): return 1000000
def wdt(p, t):
    tm = time.gmtime(t)
    mu.mem_write(p, struct.pack('<6HI', tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, int((t % 1) * 1e6)))
@H('sceRtcGetCurrentClock')
def _(p, tz, *_): wdt(p, time.time()); return 0
@H('sceRtcGetTime_t')
def _(dt, pt, *_):
    y, mo, d, h, mi, se, us = struct.unpack('<6HI', mu.mem_read(dt, 16))
    import calendar
    try: t = calendar.timegm((y, mo, d, h, mi, se, 0, 0, 0))
    except Exception: t = 0
    w32(pt, t); return 0
@H('sceRtcSetTime_t')
def _(dt, t, *_): wdt(dt, t); return 0
@H('sceRtcGetTick')
def _(dt, pt, *_):
    y, mo, d, h, mi, se, us = struct.unpack('<6HI', mu.mem_read(dt, 16))
    import calendar
    try: t = calendar.timegm((y, mo, d, h, mi, se, 0, 0, 0))
    except Exception: t = 0
    w64(pt, int((t + 62135596800) * 1e6) + us); return 0
@H('sceRtcSetTick')
def _(dt, pt, *_):
    tick = struct.unpack('<Q', mu.mem_read(pt, 8))[0]; wdt(dt, tick / 1e6 - 62135596800); return 0
@H('sceRtcConvertUtcToLocalTime')
def _(a, b, *_): mu.mem_write(b, bytes(mu.mem_read(a, 8))); return 0

# ---------------------------------------------------------------- fibers
class Ctx: pass
REGS = [UC_ARM_REG_R0 + i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_FPSCR] + \
       [UC_ARM_REG_D0 + i for i in range(32)]
def save_ctx():
    return [mu.reg_read(r) for r in REGS]
CLOBBER = os.environ.get('VEMU_CLOBBER', '1') == '1'
CALLEE = set([UC_ARM_REG_R0 + i for i in range(4, 12)] + [UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_FPSCR] + [UC_ARM_REG_D0 + i for i in range(8, 16)])
def load_ctx(c):
    for r, v in zip(REGS, c):
        if r == UC_ARM_REG_PC: continue
        if CLOBBER and r not in CALLEE:
            v = 0xdeadbeef if r < UC_ARM_REG_D0 or r > UC_ARM_REG_D31 else 0x7ff8dead0000beef
        mu.reg_write(r, v)
    mu.reg_write(UC_ARM_REG_PC, c[15] | ((c[16] >> 5) & 1))
fibers = {}
FSW = [0]
cur_fiber = [None]; thread_ctx = [None]
@H('_sceFiberInitializeImpl')
def _(fib, name, entry, arg0, ctx, size, opt):
    fibers[fib] = {'entry': entry, 'arg0': arg0, 'ctx': ctx, 'size': size, 'saved': None, 'done': False}
    # hardware SceFiber marks the lowest context word (seen in a real core dump)
    mu.mem_write(ctx, b'\xee\xee\xee\xee')
    return 0
@H('sceFiberRun')
def _(fib, argrun, pout, *_):
    f = fibers.get(fib)
    if f is None or cur_fiber[0] is not None: return 0x80590002
    mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
    mu.reg_write(UC_ARM_REG_R0, 0)
    thread_ctx[0] = (save_ctx(), pout)
    cur_fiber[0] = fib
    FSW[0] += 1
    if FSW[0] <= 40 or FSW[0] % 1000 == 0: say('[fiber] run #%d fib=%x first=%s' % (FSW[0], fib, f['saved'] is None))
    if f['saved'] is None:
        sp = (f['ctx'] + f['size']) & ~7
        mu.reg_write(UC_ARM_REG_SP, sp); mu.reg_write(UC_ARM_REG_R0, f['arg0']); mu.reg_write(UC_ARM_REG_R1, argrun)
        mu.reg_write(UC_ARM_REG_LR, MAGIC + 4)
        mu.reg_write(UC_ARM_REG_PC, f['entry'])
    else:
        ctx, fpout = f['saved']
        load_ctx(ctx)
        mu.reg_write(UC_ARM_REG_R0, 0)
        if fpout: w32(fpout, argrun)
    return RET
def fiber_return(argret, pout):
    fib = cur_fiber[0]
    f = fibers[fib]
    mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR)); mu.reg_write(UC_ARM_REG_R0, 0)
    f['saved'] = (save_ctx(), pout)
    cur_fiber[0] = None
    ctx, tpout = thread_ctx[0]
    load_ctx(ctx)
    mu.reg_write(UC_ARM_REG_R0, 0)
    if tpout: w32(tpout, argret)
@H('sceFiberReturnToThread')
def _(argret, pout, *_):
    if cur_fiber[0] is None: return 0x80590003
    fiber_return(argret, pout); return RET

# ---------------------------------------------------------------- file I/O
fds = {}
LOGBUF = ['']
CENSUS_DONE = []
CENSUS_RES = []
CENSUS_OBJS = []
def census_diff(a, b):
    import collections
    ca = collections.Counter(); cb = collections.Counter()
    for k in a: ca[k] += k[0]
    for k in b: cb[k] += k[0]
    d = collections.Counter()
    for k in set(ca) | set(cb): d[k] = cb[k] - ca[k]
    say('[census] diff total %+d KB' % (sum(d.values()) >> 10))
    # who references the biggest new objects?
    hs, objs = CENSUS_OBJS[-1]
    big = sorted(objs, key=lambda o: -o[1])[:4]
    import array
    regions = []
    for b0, n in objs:
        lay = r32(hs + b0 * 16 + n * 16 - 4)
        if lay == 0: continue  # pointer-free: the GC does not scan it
        regions.append((hs + b0 * 16, n * 16))
    for (lo, sz) in segs[1:]: regions.append((lo, sz))
    regions.append((stack, STACK_SIZE))
    for b0, n in big:
        ta = hs + b0 * 16; te = ta + n * 16
        refs = []
        for (ra, rs) in regions:
            if ra == ta: continue
            try: data = bytes(mu.mem_read(ra, rs))
            except UcError: continue
            arr = array.array('I', data[:len(data) // 4 * 4])
            for i, v in enumerate(arr):
                if ta <= v < te:
                    refs.append((ra, rs, i * 4))
                    if len(refs) > 6: break
            if len(refs) > 6: break
        say('[census] obj %x size %d refs: %s' % (ta, n * 16, ', '.join('%x(size %d)+%d lay=%x' % (r[0], r[1], r[2], r32(r[0] + r[1] - 4)) for r in refs)))
        for ra, rs, off in refs[:3]:
            say('[census]    ref obj %x first words %s' % (ra, bytes(mu.mem_read(ra, min(rs, 48))).hex()))
    for k, v in d.most_common(25):
        say('[census] diff size %d layout %s %s: %+d KB (%d objs)' % (k[0], hex(k[1]), symbolize(k[1]) if k[1] > 0x81000000 and k[1] < 0x82000000 else '', v >> 10, v // k[0]))
def heap_census():
    hs = r32(L2R(ALLSYM['runtime.heapStart'])); ms = r32(L2R(ALLSYM['runtime.metadataStart'])); eb = r32(L2R(ALLSYM['runtime.endBlock']))
    meta = bytes(mu.mem_read(ms, (eb + 3) // 4))
    def st(b):
        x = meta[b // 4] >> (b % 4); return (x & 1) | ((x >> 4) & 1) << 1
    objs = []; b = 0; cur = None
    while b < eb:
        t = st(b)
        if t in (1, 3):
            if cur: objs.append(cur)
            cur = [b, 1]
        elif t == 2 and cur: cur[1] += 1
        else:
            if cur: objs.append(cur); cur = None
        b += 1
    if cur: objs.append(cur)
    import collections
    hist = collections.Counter(); bytes_by = collections.Counter()
    for b0, n in objs:
        sz = n * 16; hist[sz] += 1; bytes_by[sz] += sz
    keys = []
    for b0, n in objs:
        lay = r32(hs + b0 * 16 + 12)
        keys.append((n * 16, lay))
    tot = sum(bytes_by.values())
    say('[census] %d objects, %d MB live' % (len(objs), tot >> 20))
    for sz, tb in bytes_by.most_common(15):
        ex = [o for o in objs if o[1] * 16 == sz][:3]
        samples = []
        for b0, n in ex:
            a = hs + b0 * 16; hdr = a + n * 16 - 8
            lay = r32(hdr + 4)
            samples.append('%x lay=%x %s' % (a, lay, bytes(mu.mem_read(a, 16)).hex()))
        say('[census] size %d x%d = %d KB | %s' % (sz, hist[sz], tb >> 10, ' ; '.join(samples)))
    CENSUS_OBJS.append((hs, objs))
    return keys
CI_HITS = set()
def ci_resolve(path):
    # Vita memory cards are case-insensitive (exFAT): emulate that.
    if os.path.exists(path) or os.environ.get('VEMU_CASESENSITIVE'): return path
    parts = path.split('/'); cur = '/' if path.startswith('/') else ''
    for i, part in enumerate(parts):
        if not part: continue
        nxt = os.path.join(cur, part)
        if not os.path.exists(nxt):
            try:
                m = [e for e in os.listdir(cur or '.') if e.lower() == part.lower()]
            except OSError:
                return path
            if not m: return path
            nxt = os.path.join(cur, m[0])
            if path not in CI_HITS:
                CI_HITS.add(path); say('[io] case-insensitive match: %s -> %s' % (part, m[0]))
        cur = nxt
    return cur
def host_path(p):
    if ':' in p:
        dev, rest = p.split(':', 1)
        return ci_resolve(os.path.join(ROOT if dev == 'ux0' else os.path.join(ROOT, '..', dev), rest.lstrip('/')))
    return ci_resolve(os.path.join(ROOT, p.lstrip('/')))
ENOENT = 0x80010002
def errno_code(e):
    import errno
    return 0x80010000 | {errno.ENOENT: 2, errno.EEXIST: 17, errno.EISDIR: 21, errno.ENOTDIR: 20, errno.EACCES: 13}.get(e.errno, 5)
@H('sceIoOpen')
def _(path, flags, mode, *_):
    p = cstr(path); hp = host_path(p)
    acc = flags & 3
    pyf = {1: os.O_RDONLY, 2: os.O_WRONLY, 3: os.O_RDWR}.get(acc, os.O_RDONLY)
    if flags & 0x100: pyf |= os.O_APPEND
    if flags & 0x200: pyf |= os.O_CREAT
    if flags & 0x400: pyf |= os.O_TRUNC
    if flags & 0x800: pyf |= os.O_EXCL
    try:
        fd = os.open(hp, pyf, 0o644)
    except OSError as e:
        if args.trace_stubs: say('[io] open %s -> fail' % p)
        return errno_code(e)
    uid = new_uid(); fds[uid] = {'fd': fd, 'path': p}
    if args.trace_stubs or os.environ.get('VEMU_OPEN'): say('[io] open %s -> %x' % (p, uid))
    if os.environ.get('VEMU_STOP_MARK') and not STOPT and os.environ['VEMU_STOP_MARK'] in p:
        import threading
        def _fire():
            STOP_REASON[0] = 'sample (stopped %ss after %s)' % (os.environ.get('VEMU_STOP_SEC', '120'), p); mu.emu_stop()
        t = threading.Timer(float(os.environ.get('VEMU_STOP_SEC', '120')), _fire); t.daemon = True; t.start(); STOPT.append(t)
    if args.press_after and PRESS_BASE[0] is None and args.press_after in p:
        PRESS_BASE[0] = FRAME[0]; say('[vemu] press base at frame %d' % FRAME[0])
    return uid
@H('sceIoClose')
def _(uid, *_):
    f = fds.pop(uid, None)
    if f: os.close(f['fd'])
    return 0
@H('sceIoRead')
def _(uid, buf, n, *_):
    f = fds.get(uid)
    if not f: return 0x80010009
    d = os.read(f['fd'], n)
    if d: mu.mem_write(buf, d)
    return len(d)
@H('sceIoWrite')
def _(uid, buf, n, *_):
    f = fds.get(uid)
    data = bytes(mu.mem_read(buf, n))
    if not f: return 0x80010009
    if os.environ.get('VEMU_STOP_MARK') and not STOPT and os.environ['VEMU_STOP_MARK'] in f['path']:
        pass
    if f['path'].endswith('cvftest.log'):
        LOGBUF[0] += data.decode('utf-8', 'replace')
        while '\n' in LOGBUF[0]:
            line, LOGBUF[0] = LOGBUF[0].split('\n', 1)
            if line: say('[log] ' + line)
            if args.press_after and PRESS_BASE[0] is None and args.press_after in line:
                PRESS_BASE[0] = FRAME[0]; say('[vemu] press base at frame %d' % FRAME[0])
            for ci, cen in enumerate(filter(None, os.environ.get('VEMU_CENSUS', '').split('|'))):
                if cen in line and ci not in CENSUS_DONE:
                    CENSUS_DONE.append(ci); CENSUS_RES.append(heap_census())
                    if len(CENSUS_RES) == 2: census_diff(*CENSUS_RES)
    return os.write(f['fd'], data)
@H('sceIoLseek')
def _(uid, _pad, offlo, offhi, *_):
    whence = arg(4)
    f = fds.get(uid)
    if not f: return 0x80010009
    off = struct.unpack('<q', struct.pack('<II', offlo, offhi))[0]
    try: r = os.lseek(f['fd'], off, whence)
    except OSError as e:
        say('[io] lseek %s off=%d whence=%d failed' % (f['path'], off, whence)); r = errno_code(e) | 0xffffffff00000000
    do_return(val64=r); return RET
@H('sceIoLseek32')
def _(uid, off, whence, *_):
    f = fds.get(uid)
    off = struct.unpack('<i', struct.pack('<I', off))[0]
    try: return os.lseek(f['fd'], off, whence)
    except OSError as e:
        say('[io] lseek32 %s off=%d whence=%d failed' % (f['path'], off, whence)); return errno_code(e)
@H('sceIoSyncByFd')
def _(*a): return 0
def write_stat(addr, st):
    mode = 0o777 & st.st_mode
    if pystat.S_ISDIR(st.st_mode): mode |= 0x1000
    else: mode |= 0x2000
    buf = struct.pack('<IIQ', mode, 0, st.st_size)
    t = time.gmtime(st.st_mtime)
    dt = struct.pack('<6HI', t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0)
    buf += dt * 3 + b'\0' * 24
    mu.mem_write(addr, buf)
@H('sceIoGetstat')
def _(path, st, *_):
    try: s = os.stat(host_path(cstr(path)))
    except OSError as e: return errno_code(e)
    write_stat(st, s); return 0
@H('sceIoGetstatByFd')
def _(uid, st, *_):
    f = fds.get(uid)
    if not f: return 0x80010009
    if 'dir' in f: s = os.stat(f['host'])
    else: s = os.fstat(f['fd'])
    write_stat(st, s); return 0
@H('sceIoChstat', 'sceIoChstatByFd', 'sceIoDevctl')
def _(*a): return 0
@H('sceIoDopen')
def _(path, *_):
    p = cstr(path); hp = host_path(p)
    try: ents = sorted(os.listdir(hp))
    except OSError as e: return errno_code(e)
    uid = new_uid(); fds[uid] = {'dir': ents, 'host': hp, 'path': p}
    return uid
@H('sceIoDread')
def _(uid, ent, *_):
    f = fds.get(uid)
    if not f or 'dir' not in f: return 0x80010009
    if not f['dir']: return 0
    n = f['dir'].pop(0)
    try: s = os.stat(os.path.join(f['host'], n))
    except OSError: s = os.lstat(os.path.join(f['host'], n))
    write_stat(ent, s)
    nb = n.encode()[:255] + b'\0'
    mu.mem_write(ent + 88, nb)
    return 1
@H('sceIoDclose')
def _(uid, *_): fds.pop(uid, None); return 0
@H('sceIoMkdir')
def _(path, mode, *_):
    try: os.mkdir(host_path(cstr(path)))
    except OSError as e: return errno_code(e)
    return 0
@H('sceIoRemove')
def _(path, *_):
    try: os.remove(host_path(cstr(path)))
    except OSError as e: return errno_code(e)
    return 0
@H('sceIoRmdir')
def _(path, *_):
    try: os.rmdir(host_path(cstr(path)))
    except OSError as e: return errno_code(e)
    return 0
@H('sceIoRename')
def _(a, b, *_):
    try: os.rename(host_path(cstr(a)), host_path(cstr(b)))
    except OSError as e: return errno_code(e)
    return 0

# ---------------------------------------------------------------- misc devices
FRAME = [0]
STOPT = []
PRESS = {}
PRESS_BASE = [None if args.press_after else 0]
for item in filter(None, args.press.split(',')):
    fr, m = item.split(':'); PRESS[int(fr)] = int(m, 0)
@H('sceCtrlPeekBufferPositive2', 'sceCtrlPeekBufferPositive', 'sceCtrlReadBufferPositive')
def _(port, data, n, *_):
    # SceCtrlData: u64 timestamp, u32 buttons, u8 lx,ly,rx,ry, ...
    btn = 0
    if PRESS_BASE[0] is not None:
        rel = FRAME[0] - PRESS_BASE[0]
        for fr, m in PRESS.items():
            if fr <= rel < fr + 6: btn |= m
    mu.mem_write(data, struct.pack('<QI4B', now_us(), btn, 128, 128, 128, 128) + b'\0' * 52)
    return 1
@H('sceCtrlGetControllerPortInfo')
def _(info, *_):
    mu.mem_write(info, bytes([0, 1, 0, 0, 0])); return 0
@H('sceAudioOutOpenPort')
def _(*a): return 1
@H('sceAudioOutOutput')
def _(*a): VTIME[0] += 5000; return 0
for n in list(stubs.values()):
    if n not in handlers:
        handlers[n] = None  # default: return 0 (logged once)

# ---------------------------------------------------------------- GL replacement (vitaGL)
GLIDS = [1]
MISSING = set()
def glid(): GLIDS[0] += 1; return GLIDS[0]
GLSTR = {}
def gl_default(name):
    return 0
# vitaGL texture model: glTexSubImage2D on a texture drawn in the last
# FRAME_PURGE_FREQ (4) frames copies the WHOLE texture to a new allocation
# (no NULL check on hardware -> crash if the pool is exhausted).
GLTEX = {'unit': 0, 'bound': {}, 'size': {}, 'last': {}, 'copy_frame': 0, 'copy_peak': 0, 'copy_total': 0, 'copies': 0, 'big': 0, 'err': 0}
GLFMT_BPP = {0x1909: 1, 0x1907: 3, 0x1908: 4, 0x190A: 2, 0x1903: 1}
def gl_tex_model(name, a):
    T = GLTEX
    if name == 'glActiveTexture': T['unit'] = a[0] - 0x84C0
    elif name == 'glBindTexture' and a[0] == 0x0DE1: T['bound'][T['unit']] = a[1]
    elif name == 'glTexImage2D' and a[0] == 0x0DE1:
        t = T['bound'].get(T['unit']); T['size'][t] = a[3] * a[4] * GLFMT_BPP.get(a[6], 4); T['last'].pop(t, None)
    elif name in ('glDrawArrays', 'glDrawElements'):
        for t in T['bound'].values(): T['last'][t] = FRAME[0]
    elif name == 'glTexSubImage2D' and a[0] == 0x0DE1:
        t = T['bound'].get(T['unit']); lf = T['last'].get(t)
        if lf is not None and FRAME[0] - lf <= 4:
            sz = T['size'].get(t, 0)
            T['copies'] += 1; T['copy_total'] += sz; T['copy_frame'] += sz
            T['last'].pop(t, None)
            if sz >= 2 << 20:
                T['big'] += 1
                if T['big'] <= 20: say('[vgl] WARNING: TexSubImage2D on in-use texture %d copies %d KB' % (t, sz >> 10))
    elif name == 'glPixelStorei' and a[0] != 0x0CF2:
        T['err'] += 1
        if T['err'] <= 3: say('[vgl] WARNING: glPixelStorei(0x%x) is GL_INVALID_ENUM on vitaGL' % a[0])
    elif name == 'vglSwapBuffers':
        T['copy_peak'] = max(T['copy_peak'], T['copy_frame']); T['copy_frame'] = 0
def gl_handler(name):
    def f(*a):
        gl_tex_model(name, a)
        if name in ('glGenTextures', 'glGenBuffers', 'glGenFramebuffers', 'glGenRenderbuffers', 'glGenVertexArrays'):
            n, p = a[0], a[1]
            for i in range(n): w32(p + 4 * i, glid())
            return 0
        if name in ('glCreateShader', 'glCreateProgram'): return glid()
        if name in ('glGetShaderiv', 'glGetProgramiv'):
            pn, p = a[1], a[2]
            w32(p, 1 if pn in (0x8B81, 0x8B82, 0x8B80) else 0); return 0
        if name in ('glGetUniformLocation', 'glGetAttribLocation'): return glid()
        if name == 'glCheckFramebufferStatus': return 0x8CD5
        if name in ('glGetIntegerv',):
            pn, p = a[0], a[1]
            vals = {0x0D33: 4096, 0x8869: 16, 0x8B4D: 16, 0x8872: 16, 0x84E8: 4096, 0x8CDF: 1, 0x0BA2: 0}
            if pn == 0x0BA2:
                mu.mem_write(p, struct.pack('<4i', 0, 0, 960, 544))
            else:
                w32(p, vals.get(pn, 0))
            return 0
        if name == 'glGetString':
            if a[0] not in GLSTR:
                GLSTR[a[0]] = alloc_misc({0x1F00: b'vemu\0', 0x1F01: b'vemu GL\0', 0x1F02: b'OpenGL ES 2.0 vemu\0', 0x8B8C: b'OpenGL ES GLSL ES 1.00\0'}.get(a[0], b'GL_EXT_none\0'))
            return GLSTR[a[0]]
        if name == 'glGetError': return 0
        if name == 'glGetFloatv':
            w32(a[1], 0); return 0
        if name == 'glGetShaderInfoLog' or name == 'glGetProgramInfoLog':
            if a[2]: w32(a[2], 0)
            if a[1] and a[3]: mu.mem_write(a[3], b'\0')
            return 0
        if name == 'vglInitExtended' or name == 'vglInit': return 1
        if name == 'vglGetProcAddress':
            n = cstr(a[0]); r = byname.get(n, 0)
            if not r: MISSING.add(n); return 0
            ra = L2R(r)
            return ra | (1 if ra in THUMBSYM else 0)
        if name == 'vglSwapBuffers':
            FRAME[0] += 1; VTIME[0] += 16667
            if FRAME[0] % 60 == 0: say('[gl] frame %d' % FRAME[0])
            if args.frames and FRAME[0] >= args.frames: raise Stop()
            if args.frames_after and PRESS_BASE[0] is not None and FRAME[0] - PRESS_BASE[0] >= args.frames_after: raise Stop()
            return 0
        if name == 'glMapBufferRange' or name == 'glMapBuffer':
            return vmap(1 << 20)
        return 0
    return f
gl_hooks = {}
for a, n in syms:
    if (n.startswith('gl') and len(n) > 2 and n[2].isupper()) or n.startswith('vgl'):
        if n in byname and byname[n] == a:
            gl_hooks[L2R(a)] = n

import signal as _signal
def _sample(signum, frame):
    try:
        pc = mu.reg_read(UC_ARM_REG_PC); lr = mu.reg_read(UC_ARM_REG_LR); sp = mu.reg_read(UC_ARM_REG_SP)
        say('[sample] pc=%s lr=%s last=%s fiber=%s' % (symbolize(pc), symbolize(lr), handlers_cur[0], cur_fiber[0]))
        # walk stack words for return addresses
        out = []
        for i in range(0, 4096, 4):
            try: w = struct.unpack('<I', bytes(mu.mem_read(sp + i, 4)))[0]
            except Exception: break
            sy = symbolize(w)
            if sy and '+' in sy and not sy.startswith('0x'): out.append(sy)
            if len(out) >= 25: break
        say('[sample] stack: ' + ' | '.join(out))
    except Exception as e:
        say('[sample] err %s' % e)
_signal.signal(_signal.SIGUSR1, _sample)
# ---------------------------------------------------------------- hooks
seen_default = set()
import collections
RING = collections.deque(maxlen=40); RING_ON = []
def stub_hook(uc, address, size, ud):
    name = stubs.get(address) or gl_hooks.get(address)
    if name is None: return
    calls[name] = calls.get(name, 0) + 1
    handlers_cur[0] = name
    a = [arg(i) for i in range(7)]
    if args.trace_stubs and not name.startswith('sceClib') and not name.startswith('gl'):
        say('[stub] %s(%s) lr=%s' % (name, ', '.join(hex(x) for x in a[:4]), symbolize(mu.reg_read(UC_ARM_REG_LR))))
    if address in gl_hooks:
        r = gl_handler(name)(*a)
    else:
        h = handlers.get(name)
        if h is None:
            if name not in seen_default:
                seen_default.add(name); say('[stub] default 0: %s' % name)
            r = 0
        else:
            co = h.__code__
            r = h(*a) if co.co_flags & 0x04 and co.co_argcount == 0 else (h(*a) if co.co_flags & 0x04 else h(*a[:co.co_argcount]))
    if os.environ.get('VEMU_RING') == name and not RING_ON:
        RING_ON.append(1)
        mu.hook_add(UC_HOOK_CODE, lambda uc, ad, sz, ud: RING.append(ad))
    if r is RET: return
    do_return(r)

NATIVE = {'sceKernelLockLwMutex': 0xa0, 'sceKernelUnlockLwMutex': 0xa0, 'sceKernelTryLockLwMutex': 0xa0, 'sceKernelGetTLSAddr': 0xa8, 'sceClibMemcpy': 0, 'sceClibMemmove': 0, 'sceDmacMemcpy': 0, 'sceClibMemset': 0x3c, 'sceClibMemcmp': 0x50, 'sceClibStrcmp': 0x84}
if os.environ.get('VEMU_NATIVE', '1') == '1':
    nb = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'native.bin'), 'rb').read()
    NATIVE_BASE = vmap(PAGE); mu.mem_write(NATIVE_BASE, nb)
    mu.mem_write(NATIVE_BASE + 0xb8, struct.pack('<I', TLS_AREA))
    for a, n in list(stubs.items()):
        if n in NATIVE:
            mu.mem_write(a, struct.pack('<II', 0xE51FF004, NATIVE_BASE + NATIVE[n]))
            del stubs[a]
# Each imported stub / replaced function becomes an SVC instruction; the
# interrupt hook dispatches by address (PC writes there are reliable).
THUMBSYM = set()
for line in subprocess.run([os.environ.get('READELF', 'arm-vita-eabi-readelf'), '-sW', args.elf], capture_output=True, text=True).stdout.splitlines():
    p_ = line.split()
    if len(p_) >= 8 and p_[3] == 'FUNC':
        v = int(p_[1], 16)
        if v & 1: THUMBSYM.add(L2R(v & ~1))
for a in list(stubs):
    mu.mem_write(a, struct.pack('<I', 0xEF0000FE))  # svc #0xfe (ARM)
for a in list(gl_hooks):
    if a in THUMBSYM:
        mu.mem_write(a, struct.pack('<H', 0xDFFE))  # svc #0xfe (Thumb)
    else:
        mu.mem_write(a, struct.pack('<I', 0xEF0000FE))
def intr_hook(uc, intno, ud):
    pc = mu.reg_read(UC_ARM_REG_PC)
    thumb = (mu.reg_read(UC_ARM_REG_CPSR) >> 5) & 1
    a = pc - (2 if thumb else 4)
    if a not in stubs and a not in gl_hooks:
        a2 = pc - 4 if thumb else pc - 2
        if a2 in stubs or a2 in gl_hooks: a = a2
        else:
            STOP_REASON[0] = 'unexpected svc at %x' % a; uc.emu_stop(); return
    hook_wrap(uc, a, 0, ud)
mu.hook_add(UC_HOOK_INTR, intr_hook)
STOP_REASON = [None]
def hook_wrap(uc, ad, sz, ud):
    try:
        stub_hook(uc, ad, sz, ud)
    except Stop:
        STOP_REASON[0] = 'stop'; uc.emu_stop()
    except Exception as e:
        import traceback; traceback.print_exc()
        STOP_REASON[0] = 'pyerr'; uc.emu_stop()

TS_COUNT = {}
ALLOC_BY = collections.Counter() if False else None
def alloc_hook(uc, ad, sz, ud):
    n = mu.reg_read(UC_ARM_REG_R0)
    if FRAME[0] < int(os.environ.get('VEMU_ALLOCS_FROM', '0')): return
    ALLOCN[symbolize(mu.reg_read(UC_ARM_REG_LR)).split('+')[0]] += 1
    if n >= int(os.environ.get('VEMU_ALLOCS_MIN', '65536')):
        lr = mu.reg_read(UC_ARM_REG_LR)
        # walk one level up using the caller's lr is unreliable; record direct caller
        ALLOCS[symbolize(lr).split('+')[0]] += n
if os.environ.get('VEMU_ALLOCS'):
    import collections as _c
    ALLOCS = _c.Counter(); ALLOCN = _c.Counter()
    _a = L2R(byname['runtime.alloc'])
    mu.hook_add(UC_HOOK_CODE, alloc_hook, begin=_a, end=_a)
SIZEKINDS = {}
def trace_sym_hook(uc, ad, sz, ud):
    n = symbolize(ad); TS_COUNT[n] = TS_COUNT.get(n, 0) + 1
    if os.environ.get('VEMU_SIZEKIND') and 'RawType).Size' in n:
        r0 = mu.reg_read(UC_ARM_REG_R0)
        k = -1 if r0 == 0 or r0 & 3 else mu.mem_read(r0, 1)[0] & 31
        key = (k, symbolize(mu.reg_read(UC_ARM_REG_LR)))
        if key not in SIZEKINDS:
            SIZEKINDS[key] = 1; say('[sizekind] kind=%d lr=%s' % key)
        return
    if os.environ.get('VEMU_FBI') and 'FieldByIndex' in n:
        ln = r32(mu.reg_read(UC_ARM_REG_SP))
        say('[fbi] len=%d lr=%s idx=%s' % (ln, symbolize(mu.reg_read(UC_ARM_REG_LR)), [r32(mu.reg_read(UC_ARM_REG_R3) + 4 * i) for i in range(min(ln, 4))]))
        return
    if os.environ.get('VEMU_LASTR0'):
        LASTR0[n] = mu.reg_read(UC_ARM_REG_R0); return
    say('[trace] %s #%d lr=%s fiber=%s' % (n, TS_COUNT[n], symbolize(mu.reg_read(UC_ARM_REG_LR)), cur_fiber[0]))
LASTR0 = {}
WATCHN = [0]
def watch_hook(uc, access, addr, size, value, ud):
    WATCHN[0] += 1
    if WATCHN[0] <= 200:
        say('[watch] write %x size %d value %x pc=%s lr=%s' % (addr, size, value & 0xffffffff, symbolize(uc.reg_read(UC_ARM_REG_PC)), symbolize(uc.reg_read(UC_ARM_REG_LR))))
if os.environ.get('VEMU_WATCH'):
    _wa, _wl = [int(x, 0) for x in os.environ['VEMU_WATCH'].split(',')]
    mu.hook_add(UC_HOOK_MEM_WRITE, watch_hook, begin=_wa, end=_wa + _wl - 1)
for n in filter(None, args.trace_sym.split(',')):
    a = L2R(byname[n])
    mu.hook_add(UC_HOOK_CODE, trace_sym_hook, begin=a & ~1, end=a & ~1)

def magic_hook(uc, ad, sz, ud):
    if ad == MAGIC:
        say('[exit] _start returned r0=%d' % mu.reg_read(UC_ARM_REG_R0)); STOP_REASON[0] = 'exit'; uc.emu_stop()
    elif ad == MAGIC + 4:
        say('[fiber] entry returned')
        fiber_return(0, 0)
mu.hook_add(UC_HOOK_CODE, magic_hook, begin=MAGIC, end=MAGIC + 8)

def backtrace():
    sp = mu.reg_read(UC_ARM_REG_SP)
    out = []
    try:
        for i in range(4096):
            v = r32(sp + 4 * i)
            if TEXT_LO + 0x14b000 <= v < TEXT_HI:
                # plausibly a return address: previous instruction is a bl/blx?
                out.append(v)
                if len(out) >= 40: break
    except UcError:
        pass
    return out

def dump(reason):
    if LASTR0:
        for k, v in LASTR0.items():
            say('[lastr0] %s r0=%x words=%s' % (k, v, [hex(r32(v + 4 * i)) for i in range(20)]))
    say('==== %s ====' % reason)
    pc = mu.reg_read(UC_ARM_REG_PC); lr = mu.reg_read(UC_ARM_REG_LR)
    say('pc=%08x %s' % (pc, symbolize(pc))); say('lr=%08x %s' % (lr, symbolize(lr)))
    say('sp=%08x cpsr=%08x fiber=%s' % (mu.reg_read(UC_ARM_REG_SP), mu.reg_read(UC_ARM_REG_CPSR), cur_fiber[0]))
    say(' '.join('r%d=%08x' % (i, mu.reg_read(UC_ARM_REG_R0 + i)) for i in range(13)))
    for v in backtrace(): say('  bt %08x %s' % (v, symbolize(v)))
    for v in RING: say('  ring %08x %s' % (v, symbolize(v)))

def mem_bad(uc, access, address, size, value, ud):
    STOP_REASON[0] = 'memfault access=%d addr=%08x' % (access, address)
    return False
mu.hook_add(UC_HOOK_MEM_INVALID, mem_bad)

# ---------------------------------------------------------------- run
say('[vemu] entry %s, %d stubs, %d gl hooks, root %s' % (symbolize(e_entry), len(stubs), len(gl_hooks), ROOT))
mu.reg_write(UC_ARM_REG_LR, MAGIC)
mu.reg_write(UC_ARM_REG_R0, 0); mu.reg_write(UC_ARM_REG_R1, 0)
start = L2R(e_entry)
t_end = time.time() + args.max_sec
pc = start
while True:
    try:
        mu.emu_start(pc, 0xffffffff, timeout=int(max(1, t_end - time.time()) * 1e6))
    except UcError as e:
        dump('UcError %s (%s)' % (e, STOP_REASON[0])); break
    if STOP_REASON[0]:
        if STOP_REASON[0] not in ('exit', 'stop'): dump(STOP_REASON[0])
        break
    if time.time() >= t_end:
        dump('timeout'); break
    # emu_stop from unicorn internals (e.g. pc change); continue
    pc = mu.reg_read(UC_ARM_REG_PC) | ((mu.reg_read(UC_ARM_REG_CPSR) >> 5) & 1)
    if os.environ.get('VEMU_DBGRET'): say('[dbg] restart %x' % pc)
say('[vemu] top calls: ' + ', '.join('%s=%d' % kv for kv in sorted(calls.items(), key=lambda x: -x[1])[:25]))
for fib, f in fibers.items():
    data = bytes(mu.mem_read(f['ctx'], f['size']))
    lo = next((i for i in range(64 * 4, len(data), 4) if data[i:i+4] != b'\0\0\0\0'), len(data))
    say('[vemu] fiber %x ctx %x size %d: deepest use %d bytes' % (fib, f['ctx'], f['size'], f['size'] - lo))
say('[vemu] GL procs missing: %d' % len(MISSING))
say('[vgl] in-use texture copies: %d (%d MB total, peak %d KB in one frame, %d >= 2 MB)' % (GLTEX['copies'], GLTEX['copy_total'] >> 20, GLTEX['copy_peak'] >> 10, GLTEX['big']))
say('[vemu] ctrl calls: ' + ', '.join('%s=%d' % kv for kv in calls.items() if 'Ctrl' in kv[0] or 'Hid' in kv[0]))
if os.environ.get('VEMU_ALLOCS'):
    for k, v in ALLOCS.most_common(30): say('[alloc] %8d KB %s' % (v >> 10, k))
    for k, v in ALLOCN.most_common(40): say('[allocn] %8d %s' % (v, k))
say('[vemu] frames=%d mem=%dMB' % (FRAME[0], MEM_BUDGET[0] >> 20))
