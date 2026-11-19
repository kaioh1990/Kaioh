// Hand-written replacements for Ikemen functions that depend on libraries
// the translation does not include (GOTOC_NATIVE_FUNCS in run_reach.sh).
#include "ik/ik_sim.h"

// decodeShiftJIS: Ikemen decodes non-UTF-8 strings from Shift_JIS
// (golang.org/x/text/encoding/japanese). Content is UTF-8 here; invalid
// strings are passed through unchanged.
// TODO(cvf): Shift_JIS table for legacy Japanese content.
GoString fn_decodeShiftJIS(GoString input) { return input; }

// cvfGcSafepoint: frame boundary for the collector (see core/gc.h).
void fn_cvfGcSafepoint(bool fighting) { gc_safepoint(fighting); }
