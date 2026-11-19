package main

import (
	"fmt"
	"go/ast"
	"go/constant"
	"go/token"
	"go/types"
	"math"
	"strconv"
	"strings"
)

func (t *Tx) exprConv(e ast.Expr, target types.Type) string {
	target = substT(target)
	if target != nil && isInvalid(target) {
		if isBindConstExpr(t, e) || !isInvalid(t.typeOf(e)) {
			return t.expr(e) // C constant expressions; typed operands of them
		}
		return "((GoOpaque){{0}}) /* unresolved type */"
	}
	tv := t.p.info.Types[e]
	if known(tv.Value) && target != nil && !goosDependent(t.p, e) {
		if isIface(target) {
			ct := defaultType(tv.Type)
			return t.box(constLit(tv.Value, ct), ct)
		}
		return constLit(tv.Value, target)
	}
	// nil to slice/map/interface
	if id, ok := e.(*ast.Ident); ok && id.Name == "nil" && target != nil {
		switch target.Underlying().(type) {
		case *types.Pointer:
			return "NULL"
		case *types.Basic: // unsafe.Pointer
			return "NULL"
		default:
			return "(" + cbase(target) + "){0}"
		}
	}
	if target != nil && isIface(target) {
		from := t.typeOf(e)
		if from != nil && !isIface(from) {
			return t.box(t.expr(e), from)
		}
	}
	return t.expr(e)
}

func constLit(v constant.Value, ty types.Type) string {
	switch v.Kind() {
	case constant.String:
		return goStrLit(constant.StringVal(v))
	case constant.Bool:
		if constant.BoolVal(v) {
			return "true"
		}
		return "false"
	}
	if ty == nil {
		return v.ExactString()
	}
	if isFloat(ty) {
		f, _ := constant.Float64Val(constant.ToFloat(v))
		if basicKind(ty) == types.Float32 {
			return floatLit(float64(float32(f)), true)
		}
		return floatLit(f, false)
	}
	if isInteger(ty) || basicKind(ty) == types.Invalid {
		vi := constant.ToInt(v)
		// int/uint are 32-bit in the C engine (as on the Vita): math.MaxInt,
		// math.MinInt and math.MaxUint keep their meaning as the type's limits
		if k := basicKind(ty); (k == types.Int || k == types.Uint || k == types.Uintptr && false) && vi.Kind() == constant.Int {
			if i, ok := constant.Int64Val(vi); ok && (i > math.MaxInt32 || i < math.MinInt32) {
				switch {
				case k == types.Uint && i > math.MaxUint32:
					return "UINT32_MAX"
				case k == types.Uint:
					// fits uint32
				case i == math.MaxInt64:
					return "INT32_MAX"
				case i == math.MinInt64:
					return "INT32_MIN"
				}
			} else if !ok && k == types.Uint {
				return "UINT32_MAX"
			}
		}
		if vi.Kind() == constant.Int {
			if i, ok := constant.Int64Val(vi); ok {
				if isUnsigned(ty) {
					return fmt.Sprintf("%dU", uint64(i))
				}
				if i == math.MinInt32 && intBits(ty) == 32 {
					return "INT32_MIN"
				}
				if intBits(ty) == 64 || i > math.MaxInt32 || i < math.MinInt32 {
					return fmt.Sprintf("%dLL", i)
				}
				return fmt.Sprint(i)
			}
			if u, ok := constant.Uint64Val(vi); ok {
				return fmt.Sprintf("%dULL", u)
			}
		}
		f, _ := constant.Float64Val(constant.ToFloat(v))
		return floatLit(f, false)
	}
	if isString(ty) {
		return goStrLit(constant.StringVal(v))
	}
	return v.ExactString()
}

func floatLit(f float64, f32 bool) string {
	if math.IsInf(f, 1) {
		return "(1.0/0.0)"
	}
	if math.IsInf(f, -1) {
		return "(-1.0/0.0)"
	}
	if math.IsNaN(f) {
		return "go_nan()"
	}
	s := strconv.FormatFloat(f, 'x', -1, 64)
	if f32 {
		return s + "f"
	}
	return s
}

func goStrLit(s string) string {
	return fmt.Sprintf("GOSTR(%s)", strconv.Quote(s))
}

func (t *Tx) expr(e ast.Expr) string {
	tv := t.p.info.Types[e]
	if known(tv.Value) && !goosDependent(t.p, e) {
		return constLit(tv.Value, tv.Type)
	}
	if g := goosRef(t.p, e); g != "" {
		return g
	}
	switch e := e.(type) {
	case *ast.Ident:
		return t.ident(e)
	case *ast.BasicLit:
		// untyped literal the checker recorded no value for (inside an
		// expression with unresolved C types): evaluate the token itself
		if v := constant.MakeFromLiteral(e.Value, e.Kind, 0); v.Kind() != constant.Unknown {
			return constLit(v, nil)
		}
		return e.Value
	case *ast.ParenExpr:
		return "(" + t.expr(e.X) + ")"
	case *ast.SelectorExpr:
		return t.selector(e)
	case *ast.CallExpr:
		return t.call(e)
	case *ast.IndexExpr:
		return t.index(e)
	case *ast.IndexListExpr:
		return t.TODO(e, "index list")
	case *ast.SliceExpr:
		return t.sliceExpr(e)
	case *ast.StarExpr:
		return "(*" + t.expr(e.X) + ")"
	case *ast.UnaryExpr:
		return t.unary(e)
	case *ast.BinaryExpr:
		return t.binary(e)
	case *ast.CompositeLit:
		return t.composite(e)
	case *ast.TypeAssertExpr:
		return t.typeAssert(e)
	case *ast.FuncLit:
		return t.funcLit(e)
	case *ast.KeyValueExpr:
		return t.TODO(e, "key/value")
	}
	return t.TODO(e, fmt.Sprintf("expr %T", e))
}

func (t *Tx) ident(e *ast.Ident) string {
	switch e.Name {
	case "nil":
		return "NULL"
	case "true", "false":
		return e.Name
	}
	obj := t.p.info.Uses[e]
	if obj == nil {
		obj = t.p.info.Defs[e]
	}
	switch o := obj.(type) {
	case *types.Var:
		if o.Pkg() != nil && o.Parent() == o.Pkg().Scope() && isOwn(o.Pkg()) {
			usedGlobals[o] = true
			return gName(o) // package-level variable
		}
		if t.captured[o] {
			return "(*_env->" + cVarName(e.Name) + ")"
		}
		if t.heap[o] {
			return "(*" + cVarName(e.Name) + "__h)"
		}
		return cVarName(e.Name)
	case *types.Func:
		return t.funcValue(o)
	case *types.Const:
		if cn, ok := bindCConst[o]; ok {
			return bindConstRef(t, o, cn)
		}
		return constLit(o.Val(), o.Type())
	case *types.Nil:
		return "NULL"
	}
	return cVarName(e.Name)
}

// x.f: field (auto-deref), package member, or method value.
func (t *Tx) selector(e *ast.SelectorExpr) string {
	if id, ok := e.X.(*ast.Ident); ok {
		if pn, ok := t.p.info.Uses[id].(*types.PkgName); ok {
			if fn, ok := t.p.info.Uses[e.Sel].(*types.Func); ok {
				if !isOwn(fn.Pkg()) {
					externs[fn] = true
				}
				return t.funcValue(fn)
			}
			if v, ok := t.p.info.Uses[e.Sel].(*types.Var); ok && isOwn(v.Pkg()) {
				usedGlobals[v] = true
				return gName(v)
			}
			if c, ok := t.p.info.Uses[e.Sel].(*types.Const); ok {
				if cn, ok := bindCConst[c]; ok { // binding constant defined in C (SDL.h)
					return bindConstRef(t, c, cn)
				}
				return constLit(c.Val(), c.Type())
			}
			return pkgPrefix(pn.Imported()) + "_" + e.Sel.Name
		}
	}
	sel := t.p.info.Selections[e]
	if sel == nil {
		return t.TODO(e, "selector")
	}
	switch sel.Kind() {
	case types.FieldVal:
		return t.fieldPath(e.X, sel)
	case types.MethodVal:
		return t.methodValue(e, sel)
	}
	return t.TODO(e, "method expr")
}

// Walks embedded fields (sel.Index()) applying -> or . as needed.
func (t *Tx) fieldPath(x ast.Expr, sel *types.Selection) string {
	base := t.expr(x)
	cur := t.typeOf(x)
	for _, idx := range sel.Index() {
		st := cur
		ptr := false
		if p, ok := st.Underlying().(*types.Pointer); ok {
			ptr = true
			st = p.Elem()
		}
		s := st.Underlying().(*types.Struct)
		f := s.Field(idx)
		if c := cbase(st); strings.HasPrefix(c, "GoOpaque") {
			// field of an opaque (platform library) struct: a zeroed scratch slot
			ft := sel.Type()
			return fmt.Sprintf("(*(%s *)go_scratch(sizeof(%s)) /* opaque %s.%s */)", cbase(ft), cbase(ft), types.TypeString(st, func(p *types.Package) string { return p.Name() }), f.Name())
		}
		if ptr {
			base = base + "->" + embName(f)
		} else {
			base = base + "." + embName(f)
		}
		cur = f.Type()
	}
	return base
}

func (t *Tx) unary(e *ast.UnaryExpr) string {
	if e.Op == token.ARROW {
		return t.recvExpr(e)
	}
	x := t.expr(e.X)
	switch e.Op {
	case token.NOT:
		return "(!" + x + ")"
	case token.SUB:
		return "(-" + x + ")"
	case token.XOR:
		return "(~" + x + ")"
	case token.AND:
		if cl, ok := e.X.(*ast.CompositeLit); ok {
			// &T{...}: heap object
			ty := t.typeOf(cl)
			return fmt.Sprintf("%s(%s, %s)", boxFn(ty), cbase(ty), t.composite(cl))
		}
		return "(&" + x + ")"
	case token.ADD:
		return x
	}
	return t.TODO(e, "unary "+e.Op.String())
}

func (t *Tx) binary(e *ast.BinaryExpr) string {
	lt := t.typeOf(e.X)
	rt := t.typeOf(e.Y)
	switch e.Op {
	case token.LAND, token.LOR:
		return "(" + t.expr(e.X) + " " + e.Op.String() + " " + t.expr(e.Y) + ")"
	}
	if (isString(lt) || isString(rt)) && !isIface(lt) && !isIface(rt) {
		switch e.Op {
		case token.ADD:
			return "go_strcat(" + t.expr(e.X) + ", " + t.expr(e.Y) + ")"
		case token.EQL:
			return "go_streq(" + t.expr(e.X) + ", " + t.expr(e.Y) + ")"
		case token.NEQ:
			return "(!go_streq(" + t.expr(e.X) + ", " + t.expr(e.Y) + "))"
		case token.LSS, token.GTR, token.LEQ, token.GEQ:
			return "(go_strcmp(" + t.expr(e.X) + ", " + t.expr(e.Y) + ") " + e.Op.String() + " 0)"
		}
	}
	// operand types for untyped constants: use the other side
	opType := lt
	if known(t.p.info.Types[e.X].Value) && rt != nil {
		opType = rt
	}
	if opType == nil {
		opType = rt
	}
	if opType == nil {
		return t.TODO(e, "binary: unknown operand types")
	}
	switch e.Op {
	case token.SHL, token.SHR:
		op := "shl"
		if e.Op == token.SHR {
			op = "shr"
		}
		return t.shift(op, t.expr(e.X), lt, e.Y)
	case token.AND_NOT:
		return "(" + t.exprConv(e.X, opType) + " & ~" + t.exprConv(e.Y, opType) + ")"
	case token.QUO:
		if isInteger(opType) {
			return fmt.Sprintf("go_div%s(%s, %s)", intSuffix(opType), t.exprConv(e.X, opType), t.exprConv(e.Y, opType))
		}
	case token.REM:
		if isInteger(opType) {
			return fmt.Sprintf("go_rem%s(%s, %s)", intSuffix(opType), t.exprConv(e.X, opType), t.exprConv(e.Y, opType))
		}
	case token.EQL, token.NEQ:
		// struct/array comparisons
		_, isArr := opType.Underlying().(*types.Array)
		if _, ok := opType.Underlying().(*types.Struct); ok || isArr {
			op := ""
			if e.Op == token.NEQ {
				op = "!"
			}
			return fmt.Sprintf("(%s%s(%s, %s))", op, eqFuncName(opType), t.exprConv(e.X, opType), t.exprConv(e.Y, opType))
		}
		op := "=="
		if e.Op == token.NEQ {
			op = "!="
		}
		isNil := func(x ast.Expr) bool {
			id, ok := x.(*ast.Ident)
			return ok && id.Name == "nil" && t.p.info.Uses[id] == types.Universe.Lookup("nil")
		}
		// comparisons with nil
		if isNil(e.X) || isNil(e.Y) {
			other, ot := e.X, lt
			if isNil(e.X) {
				other, ot = e.Y, rt
			}
			field := ""
			switch ot.Underlying().(type) {
			case *types.Interface:
				field = ".type"
			case *types.Map:
				field = ".m"
			case *types.Slice:
				field = ".p"
			case *types.Signature:
				field = ".fn"
			}
			if field != "" {
				return "(" + t.expr(other) + field + " " + op + " 0)"
			}
			if _, isChan := ot.Underlying().(*types.Chan); isChan {
				return "(" + t.expr(other) + " " + op + " NULL)"
			}
			if ot == nil || types.TypeString(ot, qual) == "invalid type" || strings.Contains(cbase(ot), "invalid type") {
				if op == "==" {
					return "true /* unresolved type: nil */"
				}
				return "false /* unresolved type: nil */"
			}
		}
		if isIface(lt) || isIface(rt) {
			it := lt
			if !isIface(lt) {
				it = rt
			}
			return "(go_iface_eq(" + t.exprConv(e.X, it) + ", " + t.exprConv(e.Y, it) + ") " + op + " true)"
		}
	}
	return "(" + t.exprConv(e.X, opType) + " " + e.Op.String() + " " + t.exprConv(e.Y, opType) + ")"
}

func intSuffix(ty types.Type) string {
	u := ""
	if isUnsigned(ty) {
		u = "u"
	}
	return fmt.Sprintf("%s%d", u, intBits(ty))
}

func (t *Tx) shift(op, x string, xt types.Type, count ast.Expr) string {
	if tv := t.p.info.Types[count]; known(tv.Value) {
		c, _ := constant.Int64Val(constant.ToInt(tv.Value))
		if c < int64(intBits(xt)) {
			if op == "shl" {
				return fmt.Sprintf("(%s)((%s) << %d)", cbase(xt), x, c)
			}
			return fmt.Sprintf("(%s)((%s) >> %d)", cbase(xt), x, c)
		}
	}
	return fmt.Sprintf("go_%s%s(%s, %s)", op, intSuffix(xt), x, t.expr(count))
}

func (t *Tx) index(e *ast.IndexExpr) string {
	xt := t.typeOf(e.X)
	if xt == nil {
		return t.TODO(e, "index of unknown")
	}
	// generic function instantiation f[T]
	if _, ok := t.p.info.Instances[identOf(e.X)]; ok {
		return t.expr(e.X)
	}
	switch u := xt.Underlying().(type) {
	case *types.Slice:
		return fmt.Sprintf("%s.p[%s]", t.expr(e.X), t.expr(e.Index))
	case *types.Array:
		return fmt.Sprintf("%s.v[%s]", t.expr(e.X), t.expr(e.Index))
	case *types.Pointer:
		return fmt.Sprintf("%s->v[%s]", t.expr(e.X), t.expr(e.Index))
	case *types.Map:
		return t.mapGet(e.X, e.Index, u, "NULL")
	case *types.Basic:
		return fmt.Sprintf("((u8)%s.p[%s])", t.expr(e.X), t.expr(e.Index))
	}
	return t.TODO(e, "index kind")
}

func identOf(e ast.Expr) *ast.Ident {
	switch x := e.(type) {
	case *ast.Ident:
		return x
	case *ast.SelectorExpr:
		return x.Sel
	}
	return nil
}

func (t *Tx) mapGet(m, k ast.Expr, mt *types.Map, okPtr string) string {
	name := cbase(mt)
	return fmt.Sprintf("%s_get(%s, %s, %s)", name, t.mapAddr(m), t.exprConv(k, mt.Key()), okPtr)
}

func (t *Tx) mapSet(m, k ast.Expr, mt *types.Map, val string) string {
	name := cbase(mt)
	return fmt.Sprintf("%s_set(%s, %s, %s)", name, t.mapAddr(m), t.exprConv(k, mt.Key()), val)
}

// mapAddr: pointer to a map handle. Map values are handles to shared
// storage, so a temporary copy is as good as the original.
func (t *Tx) mapAddr(m ast.Expr) string {
	switch m.(type) {
	case *ast.Ident, *ast.SelectorExpr, *ast.StarExpr:
		return "&" + t.expr(m)
	case *ast.IndexExpr:
		if _, isMap := t.typeOf(m.(*ast.IndexExpr).X).Underlying().(*types.Map); !isMap {
			return "&" + t.expr(m)
		}
	}
	return fmt.Sprintf("GO_TMPADDR(%s, %s)", cbase(t.typeOf(m).Underlying()), t.expr(m))
}

func (t *Tx) sliceExpr(e *ast.SliceExpr) string {
	xt := t.typeOf(e.X)
	lo, hi := "0", "-1"
	if e.Low != nil {
		lo = t.expr(e.Low)
	}
	if e.High != nil {
		hi = t.expr(e.High)
	}
	if e.Slice3 { // s[lo:hi:max]: capacity max-lo
		x := t.expr(e.X)
		mx := t.expr(e.Max)
		var st string
		switch u := xt.Underlying().(type) {
		case *types.Slice:
			st = cbase(xt)
		case *types.Array:
			st = cbase(types.NewSlice(u.Elem()))
			x = "((" + st + "){" + x + ".v, " + fmt.Sprint(u.Len()) + ", " + fmt.Sprint(u.Len()) + "})"
		case *types.Pointer:
			a := u.Elem().Underlying().(*types.Array)
			st = cbase(types.NewSlice(a.Elem()))
			x = "((" + st + "){" + x + "->v, " + fmt.Sprint(a.Len()) + ", " + fmt.Sprint(a.Len()) + "})"
		default:
			return t.TODO(e, "3-index slice")
		}
		return fmt.Sprintf("({ %s _s3 = %s; int _l3 = %s, _m3 = %s; (%s){_s3.p + _l3, (%s) - _l3, _m3 - _l3}; })", st, x, lo, mx, st, hi)
	}
	switch u := xt.Underlying().(type) {
	case *types.Slice:
		return fmt.Sprintf("GO_SLICE(%s, %s, %s, %s)", cbase(xt), t.expr(e.X), lo, hi)
	case *types.Basic:
		return fmt.Sprintf("go_substr(%s, %s, %s)", t.expr(e.X), lo, hi)
	case *types.Array:
		st := types.NewSlice(u.Elem())
		return fmt.Sprintf("GO_ARRSLICE(%s, %s.v, %d, %s, %s)", cbase(st), t.expr(e.X), u.Len(), lo, hi)
	case *types.Pointer:
		if a, ok := u.Elem().Underlying().(*types.Array); ok {
			st := types.NewSlice(a.Elem())
			return fmt.Sprintf("GO_ARRSLICE(%s, %s->v, %d, %s, %s)", cbase(st), t.expr(e.X), a.Len(), lo, hi)
		}
	}
	return t.TODO(e, "slice expr")
}

func (t *Tx) composite(e *ast.CompositeLit) string {
	ty := t.typeOf(e)
	if n, ok := ty.(*types.Named); ok && n.Obj().Pkg() != nil && !ownTypes(n.Obj().Pkg()) {
		if _, isStruct := n.Underlying().(*types.Struct); isStruct {
			// standard-library struct values are opaque in C
			if _, mapped := extStructs[types.TypeString(n, qual)]; mapped {
				goto normal
			}
			if n.Obj().Pkg().Path() == "sync" && n.Obj().Name() == "Pool" {
				for _, el := range e.Elts {
					if kv, ok := el.(*ast.KeyValueExpr); ok && kv.Key.(*ast.Ident).Name == "New" {
						return "go_sync_pool(" + t.expr(kv.Value) + ")"
					}
				}
			}
			return "((GoOpaque){{0}})"
		}
	}
normal:
	switch u := ty.Underlying().(type) {
	case *types.Struct:
		var parts []string
		for i, el := range e.Elts {
			if kv, ok := el.(*ast.KeyValueExpr); ok {
				name := kv.Key.(*ast.Ident).Name
				var ft types.Type
				cname := cField(name)
				for k := 0; k < u.NumFields(); k++ {
					if u.Field(k).Name() == name {
						ft = u.Field(k).Type()
						cname = embName(u.Field(k))
					}
				}
				parts = append(parts, "."+cname+" = "+t.exprConv(kv.Value, ft))
			} else {
				parts = append(parts, t.exprConv(el, u.Field(i).Type()))
			}
		}
		if len(parts) == 0 {
			return "(" + cbase(ty) + "){0}"
		}
		return "(" + cbase(ty) + "){" + strings.Join(parts, ", ") + "}"
	case *types.Array:
		var parts []string
		for _, el := range e.Elts {
			if kv, ok := el.(*ast.KeyValueExpr); ok {
				parts = append(parts, "["+t.expr(kv.Key)+"] = "+t.exprConv(kv.Value, u.Elem()))
			} else {
				parts = append(parts, t.exprConv(el, u.Elem()))
			}
		}
		return "(" + cbase(ty) + "){{" + strings.Join(parts, ", ") + "}}"
	case *types.Slice:
		var parts []string
		for _, el := range e.Elts {
			parts = append(parts, t.exprConv(el, u.Elem()))
		}
		if len(parts) == 0 {
			return "(" + cbase(ty) + "){0}"
		}
		return fmt.Sprintf("GO_SLICE_LIT(%s, %s, %d, %s)", cbase(u), cbase(u.Elem()), len(parts), strings.Join(parts, ", "))
	case *types.Map:
		mn := cbase(u)
		if len(e.Elts) == 0 {
			return mn + "_make(0)"
		}
		// GNU statement expression: build the map in place
		var b strings.Builder
		fmt.Fprintf(&b, "({ %s _m = %s_make(%d);", mn, mn, len(e.Elts))
		for _, el := range e.Elts {
			kv := el.(*ast.KeyValueExpr)
			k := t.compositeElem(kv.Key, u.Key())
			v := t.compositeElem(kv.Value, u.Elem())
			fmt.Fprintf(&b, " %s_set(&_m, %s, %s);", mn, k, v)
		}
		fmt.Fprintf(&b, " (%s)_m; })", cbase(ty))
		return b.String()
	}
	if strings.Contains(cbase(ty), "invalid type") {
		return "((GoOpaque){{0}}) /* unresolved type */"
	}
	return t.TODO(e, "composite")
}

func (t *Tx) compositeElem(e ast.Expr, ty types.Type) string { return t.exprConv(e, ty) }

func bindConstRef(t *Tx, c *types.Const, cn string) string {
	if isInvalid(c.Type()) { // untyped C value
		return "(" + cn + ")"
	}
	return "((" + t.ctype(c.Type()) + ")" + cn + ")"
}


// runtime.GOOS / runtime.GOARCH are constants to go/types (the translating
// machine's values); the C engine decides them at C compile time instead,
// so expressions using them are never folded (ik_goos/ik_goarch, ik_os.c).
func goosRef(p *Prog, e ast.Expr) string {
	se, ok := e.(*ast.SelectorExpr)
	if !ok {
		return ""
	}
	id, ok := se.X.(*ast.Ident)
	if !ok {
		return ""
	}
	pn, ok := p.info.Uses[id].(*types.PkgName)
	if !ok || pn.Imported().Path() != "runtime" {
		return ""
	}
	switch se.Sel.Name {
	case "GOOS":
		return "ik_goos()"
	case "GOARCH":
		return "ik_goarch()"
	}
	return ""
}

func goosDependent(p *Prog, e ast.Expr) bool {
	found := false
	ast.Inspect(e, func(n ast.Node) bool {
		if x, ok := n.(ast.Expr); ok && goosRef(p, x) != "" {
			found = true
		}
		return !found
	})
	return found
}
