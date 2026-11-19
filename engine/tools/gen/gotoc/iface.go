package main

import (
	"regexp"
	"fmt"
	"go/ast"
	"go/types"
	"sort"
	"strings"
)

// Interface values: GoIface{type id, data}. Pointer types store the
// pointer itself; other types store a pointer to a heap copy.

var typeIDs = map[string]int{}
var typeByID = map[int]types.Type{}

func typeID(ty types.Type) int {
	if ty == nil {
		return 0
	}
	ty = types.Unalias(ty)
	if isIface(ty) {
		return 0 // interfaces are never dynamic types
	}
	k := canonTypeKey(ty)
	if id, ok := typeIDs[k]; ok {
		return id
	}
	// unnamed basic types have fixed ids (= types.BasicKind) so the
	// hand-written runtime (fmt shims) can recognise them; others start at 100
	id := len(typeByID) + 100
	if b, ok := ty.(*types.Basic); ok {
		id = int(b.Kind())
	}
	typeIDs[k] = id
	typeByID[id] = ty
	return id
}

func isIface(ty types.Type) bool {
	if ty == nil {
		return false
	}
	_, ok := ty.Underlying().(*types.Interface)
	return ok
}

// box converts a concrete value expression to an interface value.
func (t *Tx) box(x string, from types.Type) string {
	if from == nil {
		return "(GoIface){0}"
	}
	if b, ok := from.(*types.Basic); ok && b.Kind() == types.UntypedNil {
		return "(GoIface){0}"
	}
	if isIface(from) {
		return x
	}
	from = defaultType(from)
	id := typeID(from)
	if _, ok := from.Underlying().(*types.Pointer); ok {
		return fmt.Sprintf("((GoIface){%d, (void *)(%s)})", id, x)
	}
	return fmt.Sprintf("((GoIface){%d, go_box(%s, %s)})", id, cbase(from), x)
}

func defaultType(ty types.Type) types.Type {
	if b, ok := ty.(*types.Basic); ok && b.Info()&types.IsUntyped != 0 {
		return types.Default(ty)
	}
	return ty
}

type ifaceMethod struct {
	iface  *types.Interface
	name   string
	method *types.Func
}

var ifaceMethods = map[string]ifaceMethod{}

func ifaceDispatcherName(name string, sig *types.Signature) string {
	// dispatchers are keyed by method name and signature
	h := uint32(2166136261)
	k := types.TypeString(sig, qual)
	for i := 0; i < len(k); i++ {
		h = (h ^ uint32(k[i])) * 16777619
	}
	return fmt.Sprintf("iface_%s_%08x", name, h)
}

func (t *Tx) ifaceCall(se *ast.SelectorExpr, fn *types.Func, args []string) string {
	sig := fn.Type().(*types.Signature)
	name := ifaceDispatcherName(fn.Name(), sig)
	if _, ok := ifaceMethods[name]; !ok {
		ifaceMethods[name] = ifaceMethod{name: fn.Name(), method: fn}
	}
	return fmt.Sprintf("%s(%s%s)", name, t.expr(se.X), prependComma(args))
}

// emitDispatchers writes one switch-based dispatcher per interface method
// used, covering every boxed concrete type that has the method.
func emitDispatchers(p *Prog) (protos []string, bodies []string) {
	var names []string
	for n := range ifaceMethods {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		im := ifaceMethods[n]
		sig := im.method.Type().(*types.Signature)
		tx := &Tx{p: p}
		ret := tx.retTypeOf(sig.Results(), n)
		params := []string{"GoIface _x"}
		var argNames []string
		for i := 0; i < sig.Params().Len(); i++ {
			params = append(params, fmt.Sprintf("%s _a%d", cbase(sig.Params().At(i).Type()), i))
			argNames = append(argNames, fmt.Sprintf("_a%d", i))
		}
		hdr := fmt.Sprintf("%s %s(%s)", ret, n, strings.Join(params, ", "))
		protos = append(protos, hdr+";")
		if dispNameCount(im.name) == 1 {
			protos = append(protos, fmt.Sprintf("#define ik_iface_%s %s", im.name, n))
		}
		var b strings.Builder
		b.WriteString(hdr + " {\n    switch (_x.type) {\n")
		if isReflectTypeMethod(im.method) {
			call := fmt.Sprintf("rt_type_%s((const RType *)_x.data%s)", im.name, prependComma(argNames))
			if ret == "void" {
				fmt.Fprintf(&b, "    case GO_TID_RTYPE: %s; return;\n", call)
			} else {
				fmt.Fprintf(&b, "    case GO_TID_RTYPE: return %s;\n", call)
			}
			rtTypeMethodsUsed[im.name] = hdr
		}
		if im.name == "Error" && ret == "GoString" {
			b.WriteString("    case GO_TID_ERRSTR: return *(GoString *)_x.data;\n")
		}
		// os.FileInfo values made by the runtime (os.Stat)
		switch {
		case im.name == "IsDir" && ret == "bool" && len(argNames) == 0:
			b.WriteString("    case GO_TID_FILEINFO: return rt_fileinfo_IsDir(_x.data);\n")
		case im.name == "Name" && ret == "GoString" && len(argNames) == 0:
			b.WriteString("    case GO_TID_FILEINFO: return rt_fileinfo_Name(_x.data);\n")
		case im.name == "Size" && ret == "i64" && len(argNames) == 0:
			b.WriteString("    case GO_TID_FILEINFO: return rt_fileinfo_Size(_x.data);\n")
		}
		var ids []int
		for id := range typeByID {
			ids = append(ids, id)
		}
		sort.Ints(ids)
		for _, id := range ids {
			ty := typeByID[id]
			ms := types.NewMethodSet(ty)
			sel := ms.Lookup(p.pkg, im.name)
			if sel == nil {
				continue
			}
			mfn, ok := sel.Obj().(*types.Func)
			if !ok || mfn.Pkg() == nil {
				continue
			}
			msig := mfn.Type().(*types.Signature)
			if !types.Identical(types.NewSignatureType(nil, nil, nil, msig.Params(), msig.Results(), msig.Variadic()),
				types.NewSignatureType(nil, nil, nil, sig.Params(), sig.Results(), sig.Variadic())) {
				continue
			}
			_, wantPtr := msig.Recv().Type().(*types.Pointer)
			_, isPtr := ty.Underlying().(*types.Pointer)
			var recv string
			switch {
			case isPtr && wantPtr:
				recv = fmt.Sprintf("(%s)_x.data", cbase(ty))
			case isPtr && !wantPtr:
				recv = fmt.Sprintf("*(%s)_x.data", cbase(ty))
			case !isPtr && wantPtr:
				recv = fmt.Sprintf("(%s *)_x.data", cbase(ty))
			default:
				recv = fmt.Sprintf("*(%s *)_x.data", cbase(ty))
			}
			call := fmt.Sprintf("%s(%s%s)", cFuncName(p, mfn), recv, prependComma(argNames))
			if ret == "void" {
				fmt.Fprintf(&b, "    case %d: %s; return;\n", id, call)
			} else {
				fmt.Fprintf(&b, "    case %d: return %s;\n", id, call)
			}
		}
		if ret == "void" {
			b.WriteString("    default: return;\n    }\n}\n")
		} else {
			fmt.Fprintf(&b, "    default: { %s z; memset(&z, 0, sizeof z); return z; }\n    }\n}\n", ret)
		}
		bodies = append(bodies, b.String())
	}
	return
}

// typeSwitch translates `switch v := x.(type)` into an if-chain on the id.
func (t *Tx) typeSwitch(s *ast.TypeSwitchStmt) {
	t.w("{")
	t.ind++
	if s.Init != nil {
		t.stmt(s.Init)
	}
	var x ast.Expr
	bind := ""
	switch a := s.Assign.(type) {
	case *ast.AssignStmt:
		x = a.Rhs[0].(*ast.TypeAssertExpr).X
		bind = a.Lhs[0].(*ast.Ident).Name
	case *ast.ExprStmt:
		x = a.X.(*ast.TypeAssertExpr).X
	}
	xv := t.newTmp()
	t.w("GoIface %s = %s;", xv, t.expr(x))
	t.w("do {")
	t.ind++
	t.inSwitch++
	defer func() { t.inSwitch-- }()
	first := true
	var deflt *ast.CaseClause
	for _, cc := range s.Body.List {
		c := cc.(*ast.CaseClause)
		if c.List == nil {
			deflt = c
			continue
		}
		var conds []string
		var single types.Type
		for _, e := range c.List {
			if id, ok := e.(*ast.Ident); ok && id.Name == "nil" {
				conds = append(conds, xv+".type == 0")
				continue
			}
			ty := substT(t.p.info.TypeOf(e))
			if isIface(ty) {
				conds = append(conds, t.implementsCond(xv, ty))
			} else {
				conds = append(conds, fmt.Sprintf("%s.type == %d", xv, typeID(ty)))
			}
			if len(c.List) == 1 {
				single = ty
			}
		}
		kw := "if"
		if !first {
			kw = "} else if"
		}
		first = false
		t.w("%s (%s) {", kw, strings.Join(conds, " || "))
		t.ind++
		if bind != "" && bind != "_" {
			if obj := t.p.info.Implicits[c]; obj != nil {
				if single != nil && !isIface(single) {
					t.w("%s", t.declObj(obj, cbase(single), t.unbox(xv, single)))
				} else {
					t.w("%s", t.declObj(obj, "GoIface", xv))
				}
			}
		}
		for _, st := range c.Body {
			t.stmt(st)
		}
		t.ind--
	}
	if deflt != nil {
		if first {
			t.w("{")
		} else {
			t.w("} else {")
		}
		t.ind++
		if bind != "" && bind != "_" && t.p.info.Implicits[deflt] != nil {
			t.w("%s", t.declObj(t.p.info.Implicits[deflt], "GoIface", xv))
		}
		for _, st := range deflt.Body {
			t.stmt(st)
		}
		t.ind--
		t.w("}")
	} else if !first {
		t.w("}")
	}
	t.ind--
	t.w("} while (0);")
	t.ind--
	t.w("}")
}

func (t *Tx) unbox(xv string, ty types.Type) string {
	if _, ok := ty.Underlying().(*types.Pointer); ok {
		return fmt.Sprintf("((%s)%s.data)", cbase(ty), xv)
	}
	return fmt.Sprintf("(*(%s *)%s.data)", cbase(ty), xv)
}

// implementsCond: x's dynamic type implements interface ty (all boxed
// types are known, so this is an id list).
func (t *Tx) implementsCond(xv string, ty types.Type) string {
	it := ty.Underlying().(*types.Interface)
	var ids []string
	for id, ct := range typeByID {
		if types.Implements(ct, it) {
			ids = append(ids, fmt.Sprintf("%s.type == %d", xv, id))
		}
	}
	sort.Strings(ids)
	if len(ids) == 0 {
		return "0"
	}
	return "(" + strings.Join(ids, " || ") + ")"
}

func (t *Tx) typeAssert(e *ast.TypeAssertExpr) string {
	ty := substT(t.p.info.TypeOf(e.Type))
	x := t.expr(e.X)
	if isIface(ty) {
		return x
	}
	typeID(ty)
	if _, ok := ty.Underlying().(*types.Pointer); ok {
		return fmt.Sprintf("((%s)(%s).data)", cbase(ty), x)
	}
	return fmt.Sprintf("(*(%s *)(%s).data)", cbase(ty), x)
}

func sortStrings(s []string) { sort.Strings(s) }

// GO_TID_* for types declared outside the package (the runtime must
// recognise *os.File etc. inside interface values).
func tidDefines(p *Prog) string {
	var out []string
	for id, ty := range typeByID {
		base := ty
		if pt, ok := ty.(*types.Pointer); ok {
			base = pt.Elem()
		}
		if s, ok := ty.(*types.Slice); ok && basicKind(s.Elem()) == types.Uint8 {
			out = append(out, fmt.Sprintf("#define GO_TID_S_uint8 %d", id))
		}
		if n, ok := base.(*types.Named); ok && n.Obj().Pkg() != nil && n.Obj().Pkg() != p.pkg {
			ts := types.TypeString(ty, func(p *types.Package) string { return p.Name() })
			ts = strings.NewReplacer("*", "P_", ".", "_", "[]", "S_").Replace(ts)
			out = append(out, fmt.Sprintf("#define GO_TID_%s %d", ts, id))
		}
	}
	sort.Strings(out)
	return strings.Join(out, "\n") + "\n"
}

// ik_tid_kind[id]: underlying basic kind of each boxed type (0 = not basic),
// for fmt's %v / %d on named numeric types.
func tidKindTable() string {
	maxID := 0
	for id := range typeByID {
		if id > maxID {
			maxID = id
		}
	}
	kinds := make([]string, maxID+1)
	for i := range kinds {
		kinds[i] = "0"
	}
	for id, ty := range typeByID {
		if b, ok := ty.Underlying().(*types.Basic); ok {
			kinds[id] = fmt.Sprint(int(b.Kind()))
		}
	}
	sizes := make([]string, maxID+1)
	for i := range sizes {
		sizes[i] = "0"
	}
	for id, ty := range typeByID {
		switch ty.Underlying().(type) {
		case *types.Pointer, *types.Signature, *types.Map, *types.Chan:
		default:
			if c := cbase(ty); !strings.Contains(c, "/*") && c != "void" {
				sizes[id] = "sizeof(" + c + ")"
			}
		}
	}
	return fmt.Sprintf("const unsigned char ik_tid_kind[%d] = {%s};\nconst unsigned ik_tid_size[%d] = {%s};\nconst int ik_tid_count = %d;", maxID+1, strings.Join(kinds, ","), maxID+1, strings.Join(sizes, ","), maxID+1)
}

func dispNameCount(name string) int {
	c := 0
	for _, im := range ifaceMethods {
		if im.name == name {
			c++
		}
	}
	return c
}

var byteRuneRe = regexp.MustCompile(`\bbyte\b|\brune\b`)

// canonTypeKey: identical types get identical keys (byte = uint8, rune = int32).
func canonTypeKey(ty types.Type) string {
	return byteRuneRe.ReplaceAllStringFunc(types.TypeString(types.Unalias(ty), qual), func(m string) string {
		if m == "byte" {
			return "uint8"
		}
		return "int32"
	})
}
