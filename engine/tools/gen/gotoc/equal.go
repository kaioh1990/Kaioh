package main

import (
	"fmt"
	"go/types"
	"sort"
	"strings"
)

// Go == on structs and arrays: field/element-wise (floats compare as
// floats: -0 == +0, NaN != NaN), strings by content, interfaces by value.
var eqFuncs = map[string]string{}

func eqFuncName(ty types.Type) string {
	name := "go_eq_" + mangle(ty)
	if _, ok := eqFuncs[name]; ok {
		return name
	}
	eqFuncs[name] = ""
	c := cbase(ty)
	var conds []string
	var walk func(a, b string, ty types.Type)
	walk = func(a, b string, ty types.Type) {
		switch u := ty.Underlying().(type) {
		case *types.Basic:
			if isString(ty) {
				conds = append(conds, fmt.Sprintf("go_streq(%s, %s)", a, b))
			} else {
				conds = append(conds, fmt.Sprintf("%s == %s", a, b))
			}
		case *types.Interface:
			conds = append(conds, fmt.Sprintf("go_iface_eq(%s, %s)", a, b))
		case *types.Array:
			for i := int64(0); i < u.Len(); i++ {
				walk(fmt.Sprintf("%s.v[%d]", a, i), fmt.Sprintf("%s.v[%d]", b, i), u.Elem())
			}
		case *types.Struct:
			for i := 0; i < u.NumFields(); i++ {
				f := u.Field(i)
				if f.Name() == "_" {
					continue
				}
				walk(a+"."+embName(f), b+"."+embName(f), f.Type())
			}
		case *types.Pointer, *types.Chan:
			conds = append(conds, fmt.Sprintf("%s == %s", a, b))
		default:
			conds = append(conds, fmt.Sprintf("memcmp(&%s, &%s, sizeof %s) == 0", a, b, a))
		}
	}
	walk("a", "b", ty)
	if len(conds) == 0 {
		conds = []string{"true"}
	}
	eqFuncs[name] = fmt.Sprintf("static inline bool %s(%s a, %s b) { (void)a; (void)b; return %s; }", name, c, c, strings.Join(conds, " && "))
	return name
}

func eqFuncLines() []string {
	var out []string
	for _, v := range eqFuncs {
		out = append(out, v)
	}
	sort.Strings(out)
	return out
}
