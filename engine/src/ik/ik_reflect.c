// reflect stand-ins: zero values / no-ops. The reflection-driven INI
// helpers (iniutils.go) and the Vita bytecode hasher are not supported yet;
// see docs/PORTING.md.
#include "ik/ik_sim.h"

GoIface reflect_Value_Interface(GoOpaque /* reflect.Value */ recv) { (void)recv; GoIface z; memset(&z, 0, sizeof z); return z; }
GoIface reflect_Value_Type(GoOpaque /* reflect.Value */ recv) { (void)recv; GoIface z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_Append(GoOpaque /* reflect.Value */ a0, Slice_GoOpaque_reflect_ValuePtr a1) { (void)a0; (void)a1; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_MakeMap(GoIface a0) { (void)a0; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_MakeSlice(GoIface a0, int a1, int a2) { (void)a0; (void)a1; (void)a2; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_New(GoIface a0) { (void)a0; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_ValueOf(GoIface a0) { (void)a0; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_Value_Elem(GoOpaque /* reflect.Value */ recv) { (void)recv; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_Value_Field(GoOpaque /* reflect.Value */ recv, int a0) { (void)recv; (void)a0; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_Value_Index(GoOpaque /* reflect.Value */ recv, int a0) { (void)recv; (void)a0; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_Value_MapIndex(GoOpaque /* reflect.Value */ recv, GoOpaque /* reflect.Value */ a0) { (void)recv; (void)a0; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* reflect.Value */ reflect_Zero(GoIface a0) { (void)a0; GoOpaque /* reflect.Value */ z; memset(&z, 0, sizeof z); return z; }
GoOpaque /* unsafe.Pointer */ reflect_Value_UnsafePointer(GoOpaque /* reflect.Value */ recv) { (void)recv; GoOpaque /* unsafe.Pointer */ z; memset(&z, 0, sizeof z); return z; }
GoString reflect_Kind_String(uintptr_t recv) { (void)recv; GoString z; memset(&z, 0, sizeof z); return z; }
GoString reflect_StructTag_Get(GoString recv, GoString a0) { (void)recv; (void)a0; GoString z; memset(&z, 0, sizeof z); return z; }
GoString reflect_Value_String(GoOpaque /* reflect.Value */ recv) { (void)recv; GoString z; memset(&z, 0, sizeof z); return z; }
Slice_GoOpaque_reflect_ValuePtr reflect_Value_MapKeys(GoOpaque /* reflect.Value */ recv) { (void)recv; Slice_GoOpaque_reflect_ValuePtr z; memset(&z, 0, sizeof z); return z; }
bool reflect_DeepEqual(GoIface a0, GoIface a1) { (void)a0; (void)a1; bool z; memset(&z, 0, sizeof z); return z; }
bool reflect_Value_Bool(GoOpaque /* reflect.Value */ recv) { (void)recv; bool z; memset(&z, 0, sizeof z); return z; }
bool reflect_Value_CanSet(GoOpaque /* reflect.Value */ recv) { (void)recv; bool z; memset(&z, 0, sizeof z); return z; }
bool reflect_Value_IsNil(GoOpaque /* reflect.Value */ recv) { (void)recv; bool z; memset(&z, 0, sizeof z); return z; }
bool reflect_Value_IsValid(GoOpaque /* reflect.Value */ recv) { (void)recv; bool z; memset(&z, 0, sizeof z); return z; }
f64 reflect_Value_Float(GoOpaque /* reflect.Value */ recv) { (void)recv; f64 z; memset(&z, 0, sizeof z); return z; }
i64 reflect_Value_Int(GoOpaque /* reflect.Value */ recv) { (void)recv; i64 z; memset(&z, 0, sizeof z); return z; }
int reflect_Value_Len(GoOpaque /* reflect.Value */ recv) { (void)recv; int z; memset(&z, 0, sizeof z); return z; }
int reflect_Value_NumField(GoOpaque /* reflect.Value */ recv) { (void)recv; int z; memset(&z, 0, sizeof z); return z; }
u64 reflect_Value_Uint(GoOpaque /* reflect.Value */ recv) { (void)recv; u64 z; memset(&z, 0, sizeof z); return z; }
uintptr_t reflect_Value_Kind(GoOpaque /* reflect.Value */ recv) { (void)recv; uintptr_t z; memset(&z, 0, sizeof z); return z; }
uintptr_t reflect_Value_Pointer(GoOpaque /* reflect.Value */ recv) { (void)recv; uintptr_t z; memset(&z, 0, sizeof z); return z; }
void reflect_Value_Set(GoOpaque /* reflect.Value */ recv, GoOpaque /* reflect.Value */ a0) { (void)recv; (void)a0; }
void reflect_Value_SetBool(GoOpaque /* reflect.Value */ recv, bool a0) { (void)recv; (void)a0; }
void reflect_Value_SetFloat(GoOpaque /* reflect.Value */ recv, f64 a0) { (void)recv; (void)a0; }
void reflect_Value_SetInt(GoOpaque /* reflect.Value */ recv, i64 a0) { (void)recv; (void)a0; }
void reflect_Value_SetMapIndex(GoOpaque /* reflect.Value */ recv, GoOpaque /* reflect.Value */ a0, GoOpaque /* reflect.Value */ a1) { (void)recv; (void)a0; (void)a1; }
void reflect_Value_SetString(GoOpaque /* reflect.Value */ recv, GoString a0) { (void)recv; (void)a0; }
void reflect_Value_SetUint(GoOpaque /* reflect.Value */ recv, u64 a0) { (void)recv; (void)a0; }
