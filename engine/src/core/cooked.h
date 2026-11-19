// Cooked character content: the states, functions, string pool and
// constants exactly as Ikemen GO's compiler produced them (written by the
// cooker, tools/ikemen_cvfcook.go). See docs/COOKED_FORMAT.md.
#ifndef CVF_COOKED_H
#define CVF_COOKED_H

#include "core/base.h"

typedef struct CvfExp {
    const u8 *ops;
    u32 len;
} CvfExp;

typedef struct CvfParam {
    u8 id;
    u32 nexp;
    CvfExp *exp;
} CvfParam;

typedef enum {
    CVF_CTL_BLOCK = 0,
    CVF_CTL_SCB = 1,
    CVF_CTL_CALLFUNC = 2,
    CVF_CTL_EXPR = 3,
    CVF_CTL_VARASSIGN = 4,
    CVF_CTL_BREAK = 5,
    CVF_CTL_CONTINUE = 6,
    CVF_CTL_NULL = 7,
    CVF_CTL_UNKNOWN = 255,
} CvfCtlKind;

struct CvfBlock;

typedef struct CvfCtrl {
    u8 kind;
    i16 sctype;        // CVF_CTL_SCB: index into cvf_sctype_names
    u32 nparams;
    CvfParam *params;  // CVF_CTL_SCB
    struct CvfBlock *block;  // CVF_CTL_BLOCK
    // CVF_CTL_CALLFUNC
    const char *fname;
    const u8 *ret;
    u32 nret;
    i32 nargs;
    // CVF_CTL_EXPR / CVF_CTL_CALLFUNC (arg) / CVF_CTL_VARASSIGN (be)
    CvfExp exp;
    u8 vari;
    const char *unknown_type;  // CVF_CTL_UNKNOWN
} CvfCtrl;

typedef struct CvfBlock {
    i32 persistent, persistent_index;
    bool ignorehitpause, ctrls_ignorehitpause;
    CvfExp trigger;
    bool loop_block, nested_in_loop, for_loop, for_assign;
    u8 for_var;
    CvfExp for_var_exp;
    CvfExp for_exp[3];
    i32 for_begin, for_end, for_increment;
    u32 nctrls;
    CvfCtrl *ctrls;
    struct CvfBlock *else_block;
} CvfBlock;

typedef struct CvfStateDef {
    i32 no;
    i32 state_type, move_type, physics, player_no;
    CvfExp state_type_exp, move_type_exp, physics_exp;
    u32 nsdparams;
    CvfParam *sdparams;  // [Statedef] parameters (stateDef_* ids)
    u32 nctrlsps;
    i32 *ctrlsps;
    i32 num_vars;
    CvfBlock block;
} CvfStateDef;

typedef struct CvfFunction {
    const char *name;
    i32 num_vars, num_args, num_rets;
    u32 nctrls;
    CvfCtrl *ctrls;
} CvfFunction;

typedef struct CvfConstant {
    const char *name;
    u8 type;  // Ikemen ValueType
    f64 num;
    const char *str;
} CvfConstant;

typedef struct CvfNamedNum {
    const char *name;
    f64 val;
} CvfNamedNum;

typedef struct CvfNumBlock {
    const char *tag;
    u32 n;
    CvfNamedNum *v;
} CvfNumBlock;

typedef struct CvfCookedChar {
    CvfArena arena;
    u8 *file;  // raw file (exp ops point into it)
    const char *def, *name, *displayname, *author;
    u16 ikemenver[3], mugenver[2];
    i32 localcoord[2];
    f32 rawlocalscl;
    i32 attack_base, defence_base;
    u32 npool;
    const char **pool;
    u32 nconst;
    CvfConstant *consts;
    CvfNumBlock data, velocity, movement;
    u32 nfuncs;
    CvfFunction *funcs;
    u32 nstates;
    CvfStateDef *states;  // sorted by number
} CvfCookedChar;

bool cvf_cooked_load(CvfCookedChar *c, const char *path);
void cvf_cooked_free(CvfCookedChar *c);
const CvfStateDef *cvf_cooked_state(const CvfCookedChar *c, i32 no);
bool cvf_cooked_num(const CvfNumBlock *b, const char *name, f64 *out);

// Controller type names as written by the cooker (Go type names).
extern const char *const cvf_sctype_names[];
extern const int cvf_sctype_count;
int cvf_sctype_index(const char *name);

#endif
