// Runtime for the translated simulation (src/ik/ik_sim.c): helpers the
// translator emits directly, plus the Go standard-library entry points
// declared in ik_sim.h ("std-library entry points") and defined in ik_rt.c.
//
// Included by ik_sim.h after the generated slice/map/tuple types.
#ifndef IK_RT_H
#define IK_RT_H

#include "core/gort.h"

// Address of a temporary copy of v (no allocation).
#define GO_TMPADDR(T, v) ((T *)(T[1]){v})

// Interface type ids: unnamed basic types use their go/types BasicKind,
// generated types start at 100 (ik_sim.h #defines GO_TID_* for the
// external types the runtime must recognise). Errors made by the runtime
// (errors.New, fmt.Errorf, strconv failures) carry a GoString*.
enum {
    GO_TID_NIL = 0,
    GO_TID_bool = 1,
    GO_TID_int = 2,
    GO_TID_i8 = 3,
    GO_TID_i16 = 4,
    GO_TID_i32 = 5,
    GO_TID_i64 = 6,
    GO_TID_uint = 7,
    GO_TID_u8 = 8,
    GO_TID_u16 = 9,
    GO_TID_u32 = 10,
    GO_TID_u64 = 11,
    GO_TID_uintptr = 12,
    GO_TID_f32 = 13,
    GO_TID_f64 = 14,
    GO_TID_string = 17,
    GO_TID_FILEINFO = 98,  // os.FileInfo made by os.Stat
    GO_TID_ERRSTR = 99,
};

GoIface go_error(GoString msg);  // a runtime error value
GoIface go_errorf(const char *fmt, ...);
GoString go_error_string(GoIface e);  // e.Error() for runtime errors; "" otherwise

// Generic slice header (every generated slice type has this layout).
typedef struct GoSliceHdr {
    void *p;
    int len, cap;
} GoSliceHdr;

// sort.Slice / sort.SliceStable on a slice header (Go's exact pdqsort, so
// orderings of equal elements match the reference).
void go_sort_slice(void *slice_hdr, size_t elem, GoFunc less);
void go_sort_slice_stable(void *slice_hdr, size_t elem, GoFunc less);

GoString go_bytes2str(Slice_u8 b);
Slice_u8 go_str2bytes(GoString s);

// Formatting (fmt verbs used by Ikemen: v d s f g e q x X c t, widths,
// precision, flags - + 0 space #).
GoString go_sprintf(GoString format, const GoIface *args, int nargs);
GoString go_sprint(const GoIface *args, int nargs, bool spaces_always);
GoString go_format_float(f64 f, char fmt, int prec, int bits);  // strconv.FormatFloat

// os.FileInfo methods for GO_TID_FILEINFO (used by generated dispatchers)
bool rt_fileinfo_IsDir(void *fi);
GoString rt_fileinfo_Name(void *fi);
i64 rt_fileinfo_Size(void *fi);

// encoding/binary.Read of count values of size bytes (big: big-endian data)
GoIface go_binread(GoIface r, void *data, size_t size, int count, int big);

// package-level variables of the standard library
extern GoIface io_EOF, io_ErrUnexpectedEOF, os_ErrNotExist;

#endif
