package main

import (
	"fmt"
	"go/types"
	"strings"
)

var pkg *types.Package

var sliceTypes = map[string]types.Type{}
var localTypes = map[string]*types.Named{}
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
	if n, ok := t.(*types.Named); ok && ownTypes(n.Obj().Pkg()) {
		pk := n.Obj().Pkg()
		pfx := ""
		if pk != pkg {
			pfx = pkgPrefix(pk) + "_"
		}
		if n.Obj().Parent() != nil && n.Obj().Parent() != pk.Scope() {
			// function-local type: unique name, body emitted by typePreamble
			pos := fmt.Sprint(n.Obj().Pos())
			h := uint32(2166136261)
			for i := 0; i < len(pos); i++ {
				h = (h ^ uint32(pos[i])) * 16777619
			}
			name := fmt.Sprintf("%s%s_L%06x", pfx, n.Obj().Name(), h&0xffffff)
			localTypes[name] = n
			return name, true
		}
		if pfx != "" {
			// types of other translated packages: bodies emitted like local types
			name := pfx + n.Obj().Name()
			if n.TypeArgs().Len() > 0 {
				if si, ok := substT(n).(*types.Named); ok {
					n = si // instance with concrete arguments (inside monomorphised code)
				}
				name += "_" + mangleTypeArgs(n)
			}
			localTypes[name] = n
			return name, true
		}
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
	if t != nil {
		t = types.Unalias(t)
	}
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
		case types.UnsafePointer:
			return "void*"
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
		switch u.Underlying().(type) {
		case *types.Interface:
			return "GoIface"
		case *types.Basic, *types.Pointer, *types.Slice, *types.Map, *types.Signature:
			return cbase(u.Underlying())
		}
		if c, ok := extStructs[types.TypeString(u, qual)]; ok {
			return c
		}
		return "GoOpaque /* " + types.TypeString(u, qual) + " */"
	case *types.Chan:
		return "GoChan*"
	case *types.TypeParam:
		if s, ok := curSubst[u]; ok {
			return cbase(s)
		}
		return "GoOpaque /* type param */"
	case *types.Struct:
		n := anonName(u)
		return n
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

var anonStructs = map[string]*types.Struct{}
var anonOrder []string

func anonName(st *types.Struct) string {
	key := types.TypeString(st, qual)
	h := uint32(2166136261)
	for i := 0; i < len(key); i++ {
		h = (h ^ uint32(key[i])) * 16777619
	}
	n := fmt.Sprintf("Anon_%08x", h)
	if _, ok := anonStructs[n]; !ok {
		anonStructs[n] = st
		anonOrder = append(anonOrder, n)
	}
	return n
}

func cField(name string) string {
	switch name {
	case "char", "int", "float", "double", "long", "short", "signed", "unsigned", "auto", "register", "static", "extern", "volatile", "const", "struct", "union", "enum", "typedef", "sizeof", "default", "case", "do", "while", "for", "if", "else", "switch", "break", "continue", "return", "goto", "void", "inline", "restrict", "bool", "true", "false",
		"stdin", "stdout", "stderr", "errno", "assert", "NULL": // C library macros
		return name + "_"
	}
	return name
}

// embName: embedded struct fields are emitted as "<Type>_" (whole value),
// with their fields promoted through an anonymous struct.
func embName(f *types.Var) string {
	if f.Embedded() {
		if _, ok := f.Type().Underlying().(*types.Struct); ok {
			return f.Name() + "_"
		}
		return "embed_" + f.Name()
	}
	return cField(f.Name())
}

// standard-library struct types given C layouts (ik_rt.h)
var extStructs = map[string]string{
	"image.Point":     "GoImagePoint",
	"image.Rectangle": "GoImageRect",
	"image.NRGBA":     "GoImageNRGBA",
	"image.Paletted":  "GoImagePaletted",
	"image.RGBA":      "GoImageRGBA",
	"image.Alpha":     "GoImageAlpha",
	"image.Uniform":   "GoImageUniform",
	"reflect.StringHeader": "GoReflectStringHeader",
	"net.UDPAddr":          "GoNetUDPAddr",
	"net.TCPAddr":          "GoNetTCPAddr",
	"net.IPNet":            "GoNetIPNet",
	"net.Interface":        "GoNetInterface",
	"net.Dialer":           "GoNetDialer",
	"http.Client":          "GoHttpClient",
	"http.Request":         "GoHttpRequest",
	"http.Response":        "GoHttpResponse",
	"url.URL":              "GoUrlURL",
	"image.Config":         "GoImageConfig",
	"io.LimitedReader":     "GoIoLimitedReader",
	"xml.Name":             "GoXmlName",
	"xml.StartElement":     "GoXmlStartElement",
	"reflect.SliceHeader":  "GoReflectSliceHeader",
	"zip.ReadCloser":  "GoZipReadCloser",
	"zip.File":        "GoZipFile",
	"reflect.StructField": "GoReflectStructField",
	"ini.LoadOptions": "GoIniLoadOptions",
}

// isInvalid: types from packages that failed to type-check (platform deps)
func isInvalid(ty types.Type) bool {
	if ty == nil {
		return true
	}
	ty = types.Unalias(ty)
	if b, ok := ty.(*types.Basic); ok && b.Kind() == types.Invalid {
		return true
	}
	if n, ok := ty.(*types.Named); ok {
		if _, isC := bindCType[n.Obj()]; isC {
			return false // binding type declared as a C type: has a C layout
		}
	}
	if b, ok := ty.Underlying().(*types.Basic); ok && b.Kind() == types.Invalid {
		return true
	}
	if p, ok := ty.(*types.Pointer); ok {
		return isInvalid(p.Elem())
	}
	return false
}

func mangleTypeArgs(n *types.Named) string {
	var parts []string
	for i := 0; i < n.TypeArgs().Len(); i++ {
		parts = append(parts, mangle(n.TypeArgs().At(i)))
	}
	return strings.Join(parts, "_")
}
