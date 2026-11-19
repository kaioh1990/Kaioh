// Minimal runtime types used by the generated Ikemen data model
// (gen_types.h): Go strings, maps, interfaces and function values.
#ifndef CVF_GORT_H
#define CVF_GORT_H

#include "core/base.h"
#include "core/anim.h"
#include "core/cooked.h"
#include "formats/air.h"

// Immutable string (arena- or content-owned bytes).
typedef struct GoString {
    const char *p;
    int len;
} GoString;

// Hash map with arbitrary key/value bytes. Implementation: core/gort.c.
typedef struct GoMap {
    void *impl;
} GoMap;

// Interface value: dynamic type id + data pointer.
typedef struct GoIface {
    int type;
    void *data;
} GoIface;

typedef struct GoFunc {
    void *fn;
    void *env;
} GoFunc;

typedef struct { u8 _opaque[64]; } GoOpaqueVal;

// Types replaced by engine-native ones.
typedef CvfAnim Animation;
typedef CvfAnimFrame AnimFrame;
typedef CvfStateDef StateBytecode;
typedef CvfBlock StateBlock;

#endif
