package main

import (
	"fmt"
	"go/types"
	"strings"
)

var pkg *types.Package

var sliceTypes = map[string]types.Type{}
var mapTypes = map[string]*types.Map{}
var arrTypes = map[string]*types.Array{}
var overrides = map[string]string{}

func fmtFloat_unused(f float64) string {
	s := fmt.Sprintf("%v", f)
	if !strings.ContainsAny(s, ".eE") {
		s += ".0"
	}
	return s
}

func isPkgNamed(t types.Type) (string, bool) {
	if n, ok := t.(*types.Named); ok && n.Obj().Pkg() == pkg {
		return n.Obj().Name(), true
	}
	return "", false
}

func namedDeps(t types.Type) []string {
	var out []string
	var walk func(t types.Type)
	seen := map[types.Type]bool{}
	walk = func(t types.Type) {
		if seen[t] {
			return
		}
		seen[t] = true
		if n, ok := isPkgNamed(t); ok {
			out = append(out, n)
			return
		}
		switch u := t.(type) {
		case *types.Pointer:
			walk(u.Elem())
		case *types.Array:
			walk(u.Elem())
		case *types.Slice:
			walk(u.Elem())
		case *types.Map:
			walk(u.Key())
			walk(u.Elem())
		case *types.Struct:
			for i := 0; i < u.NumFields(); i++ {
				walk(u.Field(i).Type())
			}
		}
	}
	walk(t)
	return out
}

func valueDeps(t types.Type) []string {
	var out []string
	switch u := t.(type) {
	case *types.Struct:
		for i := 0; i < u.NumFields(); i++ {
			out = append(out, valueDepsField(u.Field(i).Type())...)
		}
	default:
		out = append(out, valueDepsField(t)...)
	}
	return out
}

func valueDepsField(t types.Type) []string {
	if n, ok := isPkgNamed(t); ok {
		return []string{n}
	}
	switch u := t.(type) {
	case *types.Array:
		return valueDepsField(u.Elem())
	case *types.Slice:
		// slice typedefs need the element type declared (typedef only)
		return nil
	}
	return nil
}

var arrEmitted = map[string]bool{}

// ensureArrs emits the struct wrappers of array types used by a field
// (Go arrays are values: wrapping them gives C the same copy semantics).
func ensureArrs(bb *strings.Builder, t types.Type) {
	a, ok := t.(*types.Array)
	if !ok {
		if _, named := isPkgNamed(t); !named {
			if a2, ok2 := t.Underlying().(*types.Array); ok2 {
				a, ok = a2, true
			}
		}
	}
	if !ok {
		return
	}
	ensureArrs(bb, a.Elem())
	n := cbase(a)
	if arrEmitted[n] {
		return
	}
	arrEmitted[n] = true
	fmt.Fprintf(bb, "struct %s { %s v[%d]; };\n", n, cbase(a.Elem()), a.Len())
}

func cbase(t types.Type) string {
	if n, ok := isPkgNamed(t); ok {
		if o, ok := overrides[n]; ok {
			return o
		}
		return n
	}
	switch u := t.(type) {
	case *types.Basic:
		switch u.Kind() {
		case types.Bool:
			return "bool"
		case types.Int8:
			return "i8"
		case types.Int16:
			return "i16"
		case types.Int32:
			return "i32"
		case types.Int64:
			return "i64"
		case types.Int:
			return "int"
		case types.Uint8:
			return "u8"
		case types.Uint16:
			return "u16"
		case types.Uint32:
			return "u32"
		case types.Uint64:
			return "u64"
		case types.Uint, types.Uintptr:
			return "uintptr_t"
		case types.Float32:
			return "f32"
		case types.Float64:
			return "f64"
		case types.String:
			return "GoString"
		}
	case *types.Pointer:
		return cbase(u.Elem()) + "*"
	case *types.Slice:
		n := "Slice_" + mangle(u.Elem())
		sliceTypes[n] = u.Elem()
		return n
	case *types.Map:
		n := "Map_" + mangle(u.Key()) + "_" + mangle(u.Elem())
		mapTypes[n] = u
		return n
	case *types.Interface:
		return "GoIface"
	case *types.Signature:
		return "GoFunc"
	case *types.Array:
		n := fmt.Sprintf("Arr%d_%s", u.Len(), mangle(u.Elem()))
		arrTypes[n] = u
		return n
	case *types.Named:
		return "GoOpaque /* " + types.TypeString(u, qual) + " */"
	case *types.Chan:
		return "GoOpaque /* chan */"
	case *types.Struct:
		return "struct { char _anon; } /* anon struct */"
	}
	return "GoOpaque /* " + t.String() + " */"
}

func qual(p *types.Package) string {
	if p == pkg {
		return ""
	}
	return p.Name()
}

func cdecl(t types.Type, name string) string {
	return cbase(t) + " " + name
}

func mangle(t types.Type) string {
	s := cbase(t)
	r := strings.NewReplacer("*", "Ptr", " ", "", "/", "", "[", "", "]", "", ".", "_")
	if i := strings.Index(s, " /*"); i >= 0 {
		s = s[:i] + "_" + strings.Trim(r.Replace(s[i+3:]), "*")
	}
	return r.Replace(s)
}
