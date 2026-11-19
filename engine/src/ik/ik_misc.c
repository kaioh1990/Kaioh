// Small standard-library pieces: hash/fnv (32-bit FNV-1a) and
// encoding/binary Write/Size for fixed-size numbers.
#include "ik/ik_sim.h"

GoIface fnv_New32a(void) {
    u32 *h = (u32 *)gc_calloc(1, sizeof *h);
    *h = 2166136261u;
    return (GoIface){GO_TID_P_fnv_sum32a, h};
}

Ret_int__GoIface fnv_sum32a_Write(u32 *h, Slice_u8 data) {
    u32 v = *h;
    for (int i = 0; i < data.len; i++) {
        v ^= data.p[i];
        v *= 16777619u;
    }
    *h = v;
    return (Ret_int__GoIface){data.len, {0}};
}

u32 fnv_sum32a_Sum32(u32 *h) { return *h; }

Slice_u8 fnv_sum32a_Sum(u32 *h, Slice_u8 in) {
    u32 v = *h;
    u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
    for (int i = 0; i < 4; i++) in = GO_APPEND(Slice_u8, in, b[i]);
    return in;
}

// Size in bytes of a fixed-size basic value (Go basic kinds are the type ids).
static int basic_size(int tid) {
    switch (tid) {
    case 1: case 3: case 8: return 1;   // bool, int8, uint8
    case 4: case 9: return 2;           // int16, uint16
    case 5: case 10: case 13: return 4; // int32, uint32, float32
    case 6: case 11: case 14: return 8; // int64, uint64, float64
    default: return -1;
    }
}

int binary_Size(GoIface v) { return basic_size(v.type); }

GoIface binary_Write(GoIface w, GoIface order, GoIface data) {
    int n = basic_size(data.type);
    if (n < 0) return go_errorf("binary.Write: some values are not fixed-sized in type %d", data.type);
    u64 v = 0;
    memcpy(&v, data.data, (size_t)n);  // little-endian host (all supported targets)
    Slice_u8 buf = GO_MAKE(Slice_u8, u8, n, n);
    bool big = order.type == GO_TID_binary_bigEndian;
    for (int i = 0; i < n; i++) buf.p[big ? n - 1 - i : i] = (u8)(v >> (8 * i));
    Ret_int__GoIface r = iface_Write_32c5b18d(w, buf);
    return r.r1;
}
