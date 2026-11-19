// C layouts for the few standard-library struct types the translation
// accesses field by field (included by the generated ik_sim.h).
#ifndef IK_RT_TYPES_H
#define IK_RT_TYPES_H

#include "core/gen_types.h"

#ifndef CVF_DEF_Slice_u8
#define CVF_DEF_Slice_u8
typedef struct { u8 *p; int len, cap; } Slice_u8;
#endif
#ifndef CVF_DEF_Slice_GoIface
#define CVF_DEF_Slice_GoIface
typedef struct { GoIface *p; int len, cap; } Slice_GoIface;
#endif

#ifndef CVF_DEF_Slice_GoString
#define CVF_DEF_Slice_GoString
typedef struct { GoString *p; int len, cap; } Slice_GoString;
#endif

// gopkg.in/ini.v1 LoadOptions (field names as in Go)
typedef struct GoIniLoadOptions {
    bool Loose, Insensitive, InsensitiveSections, InsensitiveKeys, IgnoreContinuation, IgnoreInlineComment,
        SkipUnrecognizableLines, ShortCircuit, AllowBooleanKeys, AllowShadows, AllowNestedValues,
        AllowPythonMultilineValues, SpaceBeforeInlineComment, UnescapeValueDoubleQuotes, UnescapeValueCommentSymbols;
    Slice_GoString UnparseableSections;
    GoString KeyValueDelimiters, KeyValueDelimiterOnWrite, ChildSectionDelimiter;
    bool PreserveSurroundedQuote;
    GoFunc DebugFunc;
    int ReaderBufferSize;
    bool AllowNonUniqueSections, AllowDuplicateShadowValues;
} GoIniLoadOptions;

// reflect.StructField
typedef struct GoReflectStructField {
    GoString Name, PkgPath;
    GoIface Type;
    GoString Tag;
    uintptr_t Offset;
    struct { int *p; int len, cap; } Index;
    bool Anonymous;
} GoReflectStructField;

// image
typedef struct GoImagePoint {
    int X, Y;
} GoImagePoint;
typedef struct GoImageRect {
    GoImagePoint Min, Max;
} GoImageRect;
typedef struct GoImageNRGBA {
    Slice_u8 Pix;
    int Stride;
    GoImageRect Rect;
} GoImageNRGBA;
typedef struct GoImagePaletted {
    Slice_u8 Pix;
    int Stride;
    GoImageRect Rect;
    Slice_GoIface Palette;
} GoImagePaletted;
typedef struct GoImageRGBA {  // premultiplied
    Slice_u8 Pix;
    int Stride;
    GoImageRect Rect;
} GoImageRGBA;
typedef struct GoImageAlpha {
    Slice_u8 Pix;
    int Stride;
    GoImageRect Rect;
} GoImageAlpha;
// net, net/http, net/url, image (fields the game reads; ik_net.c, ik_http.c)
typedef struct GoNetUDPAddr {
    Slice_u8 IP;
    int Port;
    GoString Zone;
} GoNetUDPAddr;
typedef struct GoNetTCPAddr {
    Slice_u8 IP;
    int Port;
    GoString Zone;
} GoNetTCPAddr;
typedef struct GoNetIPNet {
    Slice_u8 IP, Mask;
} GoNetIPNet;
typedef struct GoNetInterface {
    int Index, MTU;
    GoString Name;
    Slice_u8 HardwareAddr;
    uintptr_t Flags;
} GoNetInterface;
typedef struct GoNetDialer {
    i64 Timeout;
} GoNetDialer;
typedef struct GoHttpClient {
    i64 Timeout;
} GoHttpClient;
typedef struct GoHttpRequest {
    GoString Method;
    GoMap Header;  // http.Header
    GoString url_;
    GoIface body_;
} GoHttpRequest;
typedef struct GoHttpResponse {
    GoString Status;
    int StatusCode;
    GoMap Header;
    GoIface Body;
} GoHttpResponse;
typedef struct GoUrlURL {
    GoString Scheme, Opaque, Host, Path, RawQuery, Fragment;
} GoUrlURL;
typedef struct GoIoLimitedReader {
    GoIface R;
    i64 N;
} GoIoLimitedReader;
typedef struct GoXmlName {
    GoString Space, Local;
} GoXmlName;
typedef struct GoXmlStartElement {
    GoXmlName Name;
    struct { void *p; int len, cap; } Attr;
} GoXmlStartElement;
typedef struct GoImageConfig {
    GoIface ColorModel;
    int Width, Height;
} GoImageConfig;

// reflect.StringHeader / SliceHeader: same layouts as GoString and slices
typedef struct GoReflectStringHeader {
    uintptr_t Data;
    int Len;
} GoReflectStringHeader;
typedef struct GoReflectSliceHeader {
    uintptr_t Data;
    int Len, Cap;
} GoReflectSliceHeader;
typedef struct GoImageUniform {
    GoIface C;
} GoImageUniform;

// archive/zip (archives are not supported: OpenReader always fails)
typedef struct GoZipFile GoZipFile;
#ifndef CVF_DEF_Slice_GoZipFilePtr
#define CVF_DEF_Slice_GoZipFilePtr
typedef struct { GoZipFile **p; int len, cap; } Slice_GoZipFilePtr;
#endif
struct GoZipFile {
    struct {
        GoString Name;
    } FileHeader_;
};
typedef struct GoZipReadCloser {
    struct {
        Slice_GoZipFilePtr File;
    } Reader_;
} GoZipReadCloser;

#endif
