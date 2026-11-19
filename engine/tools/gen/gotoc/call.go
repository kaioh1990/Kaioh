package main

import (
	"fmt"
	"go/ast"
	"go/types"
	"strings"
)

func (t *Tx) calleeFunc(c *ast.CallExpr) *types.Func {
	switch f := c.Fun.(type) {
	case *ast.Ident:
		if fn, ok := t.p.info.Uses[f].(*types.Func); ok {
			return fn
		}
	case *ast.SelectorExpr:
		if sel := t.p.info.Selections[f]; sel != nil {
			if fn, ok := sel.Obj().(*types.Func); ok {
				return fn
			}
		}
		if fn, ok := t.p.info.Uses[f.Sel].(*types.Func); ok {
			return fn
		}
	case *ast.IndexExpr:
		if id := identOf(f.X); id != nil {
			if fn, ok := t.p.info.Uses[id].(*types.Func); ok {
				return fn
			}
		}
	}
	return nil
}

func (t *Tx) call(c *ast.CallExpr) string {
	// type conversion
	if tv := t.p.info.Types[c.Fun]; tv.IsType() {
		return t.conversion(substT(tv.Type), c.Args[0])
	}
	// builtins
	if id, ok := c.Fun.(*ast.Ident); ok {
		if b, ok := t.p.info.Uses[id].(*types.Builtin); ok {
			return t.builtin(b.Name(), c)
		}
	}
	if se, ok := c.Fun.(*ast.SelectorExpr); ok {
		if b, ok := t.p.info.Uses[se.Sel].(*types.Builtin); ok {
			return t.unsafeBuiltin(b.Name(), c)
		}
	}
	fn := t.calleeFunc(c)
	if fn == nil {
		return t.indirectCall(c)
	}
	sig := fn.Type().(*types.Signature)
	if !isOwn(fn.Pkg()) && (sig.TypeParams() != nil || isGenericInstance(t, c)) {
		if r := t.special(c, fn, nil); r != "" {
			return r
		}
	}
	if (sig.TypeParams() != nil || isGenericInstance(t, c)) && !simpleGeneric[fn.Name()] && sig.Recv() == nil {
		if name, isig := t.genericCall(c, fn); name != "" {
			return name + "(" + strings.Join(t.callArgs(c, isig), ", ") + ")"
		}
	}
	args := t.callArgs(c, sig)
	// generic helpers with hand-written C versions (gort.h): Min, Max, Abs, Sign, Clamp
	if sig.TypeParams() != nil || isGenericInstance(t, c) {
		inst := t.instanceType(c)
		m := mangle(inst)
		// plain argument list (generic variadics: Min(a, b, ...), Max(...))
		var plain []string
		for _, a := range c.Args {
			plain = append(plain, t.exprConv(a, inst))
		}
		switch fn.Name() {
		case "Min", "Max":
			if c.Ellipsis.IsValid() {
				return t.TODO(c, "generic Min/Max over slice")
			}
			s := plain[0]
			for _, a := range plain[1:] {
				s = fmt.Sprintf("go_%s_%s(%s, %s)", strings.ToLower(fn.Name()), m, s, a)
			}
			return s
		}
		return fmt.Sprintf("%s_%s(%s)", cFuncName(t.p, fn), m, strings.Join(plain, ", "))
	}
	if sig.Recv() != nil {
		se, ok := c.Fun.(*ast.SelectorExpr)
		if !ok {
			return t.TODO(c, "method call form")
		}
		sel := t.p.info.Selections[se]
		if sel == nil {
			return t.TODO(c, "method expression call")
		}
		if _, isIface := sel.Recv().Underlying().(*types.Interface); isIface {
			return t.ifaceCall(se, fn, args)
		}
		recv := t.methodRecv(se.X, sel, sig)
		return fmt.Sprintf("%s(%s%s)", cFuncName(t.p, fn), recv, prependComma(args))
	}
	if !isOwn(fn.Pkg()) {
		if r := t.special(c, fn, args); r != "" {
			return r
		}
		return fmt.Sprintf("%s(%s)", cFuncName(t.p, fn), strings.Join(args, ", "))
	}
	return fmt.Sprintf("%s(%s)", cFuncName(t.p, fn), strings.Join(args, ", "))
}

func isGenericInstance(t *Tx, c *ast.CallExpr) bool {
	id := identOf(c.Fun)
	if ix, ok := c.Fun.(*ast.IndexExpr); ok {
		id = identOf(ix.X)
	}
	if id == nil {
		return false
	}
	_, ok := t.p.info.Instances[id]
	return ok
}

func (t *Tx) instanceType(c *ast.CallExpr) types.Type {
	id := identOf(c.Fun)
	if ix, ok := c.Fun.(*ast.IndexExpr); ok {
		id = identOf(ix.X)
	}
	if inst, ok := t.p.info.Instances[id]; ok && inst.TypeArgs.Len() > 0 {
		return inst.TypeArgs.At(0)
	}
	return types.Typ[types.Int]
}

func prependComma(args []string) string {
	if len(args) == 0 {
		return ""
	}
	return ", " + strings.Join(args, ", ")
}

func (t *Tx) callArgs(c *ast.CallExpr, sig *types.Signature) []string {
	var args []string
	np := sig.Params().Len()
	for i, a := range c.Args {
		var pt types.Type
		if sig.Variadic() && i >= np-1 {
			if c.Ellipsis.IsValid() {
				args = append(args, t.expr(a))
				continue
			}
			// collect the variadic tail into a slice literal
			st := sig.Params().At(np - 1).Type().(*types.Slice)
			var tail []string
			for _, a2 := range c.Args[np-1:] {
				tail = append(tail, t.exprConv(a2, st.Elem()))
			}
			args = append(args, fmt.Sprintf("GO_SLICE_LIT(%s, %s, %d, %s)", cbase(st.Underlying()), cbase(st.Elem()), len(tail), strings.Join(tail, ", ")))
			return args
		}
		if i < np {
			pt = sig.Params().At(i).Type()
		}
		args = append(args, t.exprConv(a, pt))
	}
	if sig.Variadic() && len(c.Args) == np-1 {
		st := sig.Params().At(np - 1).Type()
		args = append(args, "("+cbase(st)+"){0}")
	}
	return args
}

// Receiver expression adjusted to the method's receiver kind, following
// embedded fields when the method is promoted.
func (t *Tx) methodRecv(x ast.Expr, sel *types.Selection, sig *types.Signature) string {
	base := t.expr(x)
	cur := t.typeOf(x)
	path := sel.Index()
	for _, idx := range path[:len(path)-1] {
		st := cur
		ptr := false
		if p, ok := st.Underlying().(*types.Pointer); ok {
			ptr = true
			st = p.Elem()
		}
		f := st.Underlying().(*types.Struct).Field(idx)
		if ptr {
			base += "->" + embName(f)
		} else {
			base += "." + embName(f)
		}
		cur = f.Type()
	}
	_, wantPtr := sig.Recv().Type().(*types.Pointer)
	_, havePtr := cur.Underlying().(*types.Pointer)
	if wantPtr && !havePtr {
		return "&" + base
	}
	if !wantPtr && havePtr {
		return "*" + base
	}
	return base
}

func (t *Tx) conversion(to types.Type, arg ast.Expr) string {
	to = substT(to)
	if id, ok := arg.(*ast.Ident); ok && id.Name == "nil" {
		return t.exprConv(arg, to)
	}
	from := t.typeOf(arg)
	if tv := t.p.info.Types[arg]; tv.Value != nil {
		return constLit(tv.Value, to)
	}
	if from != nil && isInvalid(from) {
		return "((" + cbase(to) + ")0) /* from an unresolved type */"
	}
	x := t.expr(arg)
	if from == nil {
		return "((" + cbase(to) + ")" + x + ")"
	}
	switch {
	case isFloat(from) && isInteger(to):
		return fmt.Sprintf("go_f2%s(%s)", intSuffix2(to), x)
	case isInteger(from) && isFloat(to), isFloat(from) && isFloat(to), isInteger(from) && isInteger(to):
		return "((" + cbase(to) + ")" + x + ")"
	case isString(to):
		if isString(from) {
			return x
		}
		if s, ok := from.Underlying().(*types.Slice); ok && basicKind(s.Elem()) == types.Uint8 {
			return "go_bytes2str(" + x + ")"
		}
		if isInteger(from) {
			return "go_rune2str(" + x + ")"
		}
	case isString(from):
		if s, ok := to.Underlying().(*types.Slice); ok && basicKind(s.Elem()) == types.Uint8 {
			return "go_str2bytes(" + x + ")"
		}
	}
	if _, ok := to.Underlying().(*types.Interface); ok {
		return t.box(x, from)
	}
	// same underlying type: nothing to do in C (typedefs)
	if types.Identical(from.Underlying(), to.Underlying()) {
		if _, ok := to.Underlying().(*types.Basic); ok {
			return "((" + cbase(to) + ")" + x + ")"
		}
		return x
	}
	return "((" + cbase(to) + ")" + x + ")"
}

func intSuffix2(ty types.Type) string {
	if isUnsigned(ty) {
		return fmt.Sprintf("u%d", intBits(ty))
	}
	return fmt.Sprintf("i%d", intBits(ty))
}

func (t *Tx) builtin(name string, c *ast.CallExpr) string {
	switch name {
	case "len", "cap":
		xt := t.typeOf(c.Args[0])
		x := t.expr(c.Args[0])
		switch u := xt.Underlying().(type) {
		case *types.Slice:
			return x + "." + name
		case *types.Basic:
			return x + ".len"
		case *types.Array:
			return fmt.Sprint(u.Len())
		case *types.Pointer:
			if a, ok := u.Elem().Underlying().(*types.Array); ok {
				return fmt.Sprint(a.Len())
			}
		case *types.Map:
			return cbase(xt.Underlying()) + "_len(&" + x + ")"
		}
		return t.TODO(c, name)
	case "append":
		st := t.typeOf(c.Args[0])
		if c.Ellipsis.IsValid() {
			if isString(t.typeOf(c.Args[1])) {
				return fmt.Sprintf("GO_APPEND_SLICE(%s, %s, go_str2bytes(%s))", cbase(st.Underlying()), t.expr(c.Args[0]), t.expr(c.Args[1]))
			}
			return fmt.Sprintf("GO_APPEND_SLICE(%s, %s, %s)", cbase(st.Underlying()), t.expr(c.Args[0]), t.expr(c.Args[1]))
		}
		s := t.expr(c.Args[0])
		elem := st.Underlying().(*types.Slice).Elem()
		for _, a := range c.Args[1:] {
			s = fmt.Sprintf("GO_APPEND(%s, %s, %s)", cbase(st.Underlying()), s, t.exprConv(a, elem))
		}
		return s
	case "make":
		ty := t.p.info.Types[c.Args[0]].Type
		switch u := ty.Underlying().(type) {
		case *types.Slice:
			n := "0"
			cp := ""
			if len(c.Args) > 1 {
				n = t.expr(c.Args[1])
			}
			if len(c.Args) > 2 {
				cp = t.expr(c.Args[2])
			} else {
				cp = n
			}
			if pointerFree(u.Elem()) {
				return fmt.Sprintf("GO_MAKE_A(%s, %s, %s, %s)", cbase(ty), cbase(u.Elem()), n, cp)
			}
			return fmt.Sprintf("GO_MAKE(%s, %s, %s, %s)", cbase(ty), cbase(u.Elem()), n, cp)
		case *types.Map:
			n := "0"
			if len(c.Args) > 1 {
				n = t.expr(c.Args[1])
			}
			return cbase(ty.Underlying()) + "_make(" + n + ")"
		}
		return t.TODO(c, "make")
	case "new":
		ty := t.p.info.Types[c.Args[0]].Type
		if pointerFree(ty) {
			return fmt.Sprintf("go_new_atomic(%s)", cbase(ty))
		}
		return fmt.Sprintf("go_new(%s)", cbase(ty))
	case "copy":
		return fmt.Sprintf("GO_COPY(%s, %s)", t.expr(c.Args[0]), t.expr(c.Args[1]))
	case "delete":
		mt := t.typeOf(c.Args[0])
		m := mt.Underlying().(*types.Map)
		return fmt.Sprintf("%s_delete(&%s, %s)", cbase(m), t.expr(c.Args[0]), t.exprConv(c.Args[1], m.Key()))
	case "panic":
		return "go_panic(" + t.expr(c.Args[0]) + ")"
	case "min", "max":
		ty := t.typeOf(c)
		var args []string
		for _, a := range c.Args {
			args = append(args, t.exprConv(a, ty))
		}
		s := args[0]
		for _, a := range args[1:] {
			s = fmt.Sprintf("go_%s_%s(%s, %s)", name, mangle(ty), s, a)
		}
		return s
	case "clear":
		return t.TODO(c, "clear")
	case "print", "println":
		return "((void)0)"
	case "recover":
		return "((GoIface){0})" // panics abort in the C runtime
	}
	return t.TODO(c, "builtin "+name)
}

var simpleGeneric = map[string]bool{"Min": true, "Max": true, "Abs": true, "Sign": true, "Clamp": true}

func (t *Tx) unsafeBuiltin(name string, c *ast.CallExpr) string {
	switch name {
	case "StringData":
		return "((u8 *)(" + t.expr(c.Args[0]) + ").p)"
	case "SliceData":
		return "((" + t.expr(c.Args[0]) + ").p)"
	case "String":
		return fmt.Sprintf("((GoString){(const char *)(%s), (int)(%s)})", t.expr(c.Args[0]), t.expr(c.Args[1]))
	case "Slice":
		st := t.typeOf(c)
		return fmt.Sprintf("((%s){(%s), (int)(%s), (int)(%s)})", cbase(st), t.expr(c.Args[0]), t.expr(c.Args[1]), t.expr(c.Args[1]))
	case "Sizeof":
		return fmt.Sprintf("sizeof(%s)", cbase(t.typeOf(c.Args[0])))
	case "Add":
		return fmt.Sprintf("((void *)((char *)(%s) + (%s)))", t.expr(c.Args[0]), t.expr(c.Args[1]))
	}
	return t.TODO(c, "unsafe."+name)
}
