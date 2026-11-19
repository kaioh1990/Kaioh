package main

import (
	"fmt"
	"go/ast"
	"go/types"
	"sort"
	"strings"
)

// Standard-library functions and methods the translation calls. Their C
// prototypes are generated (ik_ext.h); bodies are hand-written in ik_rt.c.
var externs = map[*types.Func]bool{}

func externProtos(p *Prog) []string {
	seen := map[string]bool{}
	var out []string
	for fn := range externs {
		sig := fn.Type().(*types.Signature)
		if sig.TypeParams() != nil {
			continue
		}
		name := cFuncName(p, fn)
		if inlineProvided[name] {
			continue
		}
		if seen[name] {
			continue
		}
		seen[name] = true
		tx := &Tx{p: p}
		var ps []string
		if r := sig.Recv(); r != nil {
			ps = append(ps, cbase(r.Type())+" recv")
		}
		for i := 0; i < sig.Params().Len(); i++ {
			ps = append(ps, fmt.Sprintf("%s a%d", cbase(sig.Params().At(i).Type()), i))
		}
		if len(ps) == 0 {
			ps = []string{"void"}
		}
		out = append(out, fmt.Sprintf("%s %s(%s); // %s", tx.retTypeOf(sig.Results(), ""), name, strings.Join(ps, ", "), types.ObjectString(fn, qual)))
	}
	for _, pr := range promotedProtos {
		out = append(out, pr)
	}
	sort.Strings(out)
	return out
}

// special: calls whose C form differs from a plain call.
func (t *Tx) special(c *ast.CallExpr, fn *types.Func, args []string) string {
	if fn.Pkg().Name() == "maps" && fn.Name() == "Clear" { // maps or golang.org/x/exp/maps
		return fmt.Sprintf("gomap_clear((GoMap *)%s)", t.mapAddr(c.Args[0]))
	}
	if fn.Pkg().Name() == "maps" && fn.Name() == "Clone" {
		mt := t.typeOf(c.Args[0])
		return fmt.Sprintf("((%s)gomap_clone((GoMap *)%s))", cbase(mt), t.mapAddr(c.Args[0]))
	}
	if fn.Pkg().Name() == "slices" && fn.Name() == "Clone" {
		st := t.typeOf(c.Args[0])
		return fmt.Sprintf("GO_APPEND_SLICE(%s, (%s){0}, %s)", cbase(st.Underlying()), cbase(st.Underlying()), t.expr(c.Args[0]))
	}
	if fn.Pkg().Path() == "sort" && (fn.Name() == "Slice" || fn.Name() == "SliceStable") {
		st := t.typeOf(c.Args[0])
		x := t.expr(c.Args[0])
		f := "go_sort_slice"
		if fn.Name() == "SliceStable" {
			f = "go_sort_slice_stable"
		}
		el := st.Underlying().(*types.Slice).Elem()
		return fmt.Sprintf("%s((void *)GO_TMPADDR(%s, %s), sizeof(%s), %s)", f, cbase(st), x, cbase(el), t.exprConv(c.Args[1], nil))
	}
	if fn.Pkg().Path() == "encoding/binary" && fn.Name() == "Read" {
		big := 0
		if strings.Contains(types.ExprString(c.Args[1]), "BigEndian") {
			big = 1
		}
		r := t.exprConv(c.Args[0], types.NewInterfaceType(nil, nil))
		dt := t.typeOf(c.Args[2])
		d := t.expr(c.Args[2])
		switch u := dt.Underlying().(type) {
		case *types.Slice:
			if isBasicNum(u.Elem()) {
				return fmt.Sprintf("go_binread(%s, (void *)GO_TMPADDR(%s, %s)->p, sizeof(%s), GO_TMPADDR(%s, %s)->len, %d)", r, cbase(dt), d, cbase(u.Elem()), cbase(dt), d, big)
			}
		case *types.Pointer:
			if isBasicNum(u.Elem()) {
				return fmt.Sprintf("go_binread(%s, (void *)(%s), sizeof(%s), 1, %d)", r, d, cbase(u.Elem()), big)
			}
			return fmt.Sprintf("%s(%s, %s)", binreadHelper(u.Elem(), big), r, d)
		}
		if isIface(dt) {
			return fmt.Sprintf("ik_binread_any(%s, %s, %d)", r, d, big)
		}
		return t.TODO(c, "binary.Read of "+types.TypeString(dt, qual))
	}
	return ""
}

func isBasicNum(ty types.Type) bool {
	// encoding/binary: fixed-size numbers and bool (not int/uint/uintptr)
	b, ok := ty.Underlying().(*types.Basic)
	if !ok || b.Kind() == types.Int || b.Kind() == types.Uint || b.Kind() == types.Uintptr {
		return false
	}
	return b.Info()&(types.IsInteger|types.IsFloat) != 0 || b.Kind() == types.Bool
}

var binreadHelpers = map[string]string{}
var binreadOrder []string

// binreadHelper returns a function reading a fixed-size value field by
// field, as encoding/binary does (no padding).
func binreadHelper(ty types.Type, big int) string {
	name := fmt.Sprintf("binread_%s_%d", mangle(ty), big)
	if _, ok := binreadHelpers[name]; ok {
		return name
	}
	binreadHelpers[name] = ""
	var b strings.Builder
	fmt.Fprintf(&b, "static GoIface %s(GoIface r, %s *p) {\n    GoIface e;\n", name, cbase(ty))
	var walk func(expr string, ty types.Type)
	walk = func(expr string, ty types.Type) {
		switch u := ty.Underlying().(type) {
		case *types.Basic:
			fmt.Fprintf(&b, "    if ((e = go_binread(r, (void *)&%s, sizeof(%s), 1, %d)).type) return e;\n", expr, cbase(ty), big)
		case *types.Array:
			if isBasicNum(u.Elem()) {
				fmt.Fprintf(&b, "    if ((e = go_binread(r, (void *)%s.v, sizeof(%s), %d, %d)).type) return e;\n", expr, cbase(u.Elem()), u.Len(), big)
			} else {
				for i := int64(0); i < u.Len(); i++ {
					walk(fmt.Sprintf("%s.v[%d]", expr, i), u.Elem())
				}
			}
		case *types.Struct:
			if n, ok := ty.(*types.Named); ok && n.Obj().Pkg() != nil && !ownTypes(n.Obj().Pkg()) {
				fmt.Fprintf(&b, "    /* opaque %s */\n", types.TypeString(ty, qual))
				return // standard-library struct: opaque in C
			}
			for i := 0; i < u.NumFields(); i++ {
				f := u.Field(i)
				walk(expr+"."+embName(f), f.Type())
			}
		default:
			fmt.Fprintf(&b, "    /* unsupported field %s */\n", expr)
		}
	}
	walk("(*p)", ty)
	b.WriteString("    return (GoIface){0};\n}")
	binreadHelpers[name] = b.String()
	binreadOrder = append(binreadOrder, name)
	return name
}

var inlineProvided = map[string]bool{}

func init() {
	for _, n := range []string{"IsNaN", "IsInf", "Abs", "Floor", "Ceil", "Sqrt", "Trunc", "Round", "Max", "Min",
		"Copysign", "Signbit", "Inf", "NaN", "Float32bits", "Float32frombits", "Float64bits", "Float64frombits"} {
		inlineProvided["math_"+n] = true
	}
}

// ik_binread_any: binary.Read into a value passed as interface{} (Ikemen's
// read(x interface{}) helpers). One case per boxed fixed-size type.
func binreadAny() string {
	var ids []int
	for id := range typeByID {
		ids = append(ids, id)
	}
	sort.Ints(ids)
	var b strings.Builder
	b.WriteString("GoIface ik_binread_any(GoIface r, GoIface x, int big) {\n    switch (x.type) {\n")
	for _, id := range ids {
		ty := typeByID[id]
		switch u := ty.Underlying().(type) {
		case *types.Pointer:
			if isBasicNum(u.Elem()) {
				fmt.Fprintf(&b, "    case %d: return go_binread(r, x.data, sizeof(%s), 1, big);\n", id, cbase(u.Elem()))
			} else if fixedSize(u.Elem()) {
				fmt.Fprintf(&b, "    case %d: return big ? %s(r, x.data) : %s(r, x.data);\n", id, binreadHelper(u.Elem(), 1), binreadHelper(u.Elem(), 0))
			}
		case *types.Slice:
			if isBasicNum(u.Elem()) {
				fmt.Fprintf(&b, "    case %d: return go_binread(r, ((GoSliceHdr *)x.data)->p, sizeof(%s), ((GoSliceHdr *)x.data)->len, big);\n", id, cbase(u.Elem()))
			}
		}
	}
	b.WriteString("    }\n    return go_error(GOSTR(\"binary.Read: invalid type\"));\n}\n")
	return b.String()
}

func fixedSize(ty types.Type) bool {
	switch u := ty.Underlying().(type) {
	case *types.Basic:
		return isBasicNum(ty)
	case *types.Array:
		return fixedSize(u.Elem())
	case *types.Struct:
		for i := 0; i < u.NumFields(); i++ {
			if !fixedSize(u.Field(i).Type()) {
				return false
			}
		}
		return true
	}
	return false
}
