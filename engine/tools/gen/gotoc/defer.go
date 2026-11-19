package main

import (
	"fmt"
	"go/ast"
	"go/token"
	"go/types"
	"strings"
)

// ---------------------------------------------------------------- heap vars
// Locals captured by closures are heap-allocated (Go's escape analysis does
// the same), so closures that outlive their function stay valid.
//
// Variables captured only by non-escaping literals (see escape.go) and never
// address-taken are "stack boxes": same pointer-based access, C stack storage.
func heapVarsOf(p *Prog, region ast.Node) (map[types.Object]bool, map[types.Object]bool, map[*ast.FuncLit]bool) {
	out := map[types.Object]bool{}
	nonEsc := nonEscapingLits(p, region)
	// locals whose address is taken (&x, &x.f, &x[i] on arrays) may outlive
	// the frame (Go moves them to the heap): allocate them on the heap too
	ast.Inspect(region, func(n ast.Node) bool {
		if fl, ok := n.(*ast.FuncLit); ok && fl != region {
			return false // a nested literal's own locals are handled with it
		}
		u, ok := n.(*ast.UnaryExpr)
		if !ok || u.Op != token.AND {
			return true
		}
		x := u.X
		for {
			switch e := x.(type) {
			case *ast.ParenExpr:
				x = e.X
				continue
			case *ast.SelectorExpr:
				if sel := p.info.Selections[e]; sel != nil && sel.Kind() == types.FieldVal {
					if _, isPtr := p.info.TypeOf(e.X).Underlying().(*types.Pointer); !isPtr {
						x = e.X
						continue
					}
				}
			case *ast.IndexExpr:
				if _, isArr := p.info.TypeOf(e.X).Underlying().(*types.Array); isArr {
					x = e.X
					continue
				}
			}
			break
		}
		if id, ok := x.(*ast.Ident); ok {
			if v, ok := p.info.Uses[id].(*types.Var); ok && !v.IsField() && v.Parent() != p.pkg.Scope() && v.Pkg() == p.pkg &&
				v.Pos() >= region.Pos() && v.Pos() <= region.End() {
				out[v] = true
			}
		}
		return true
	})
	addr := map[types.Object]bool{}
	for o := range out {
		addr[o] = true
	}
	escapes := map[types.Object]bool{}
	var lits []*ast.FuncLit
	var walk func(n ast.Node)
	walk = func(n ast.Node) {
		ast.Inspect(n, func(m ast.Node) bool {
			if fl, ok := m.(*ast.FuncLit); ok && fl != region && m != n {
				lits = append(lits, fl)
				walk(fl.Body)
				lits = lits[:len(lits)-1]
				return false
			}
			id, ok := m.(*ast.Ident)
			if !ok || len(lits) == 0 {
				return true
			}
			v, ok := p.info.Uses[id].(*types.Var)
			if !ok || v.IsField() || v.Parent() == p.pkg.Scope() || v.Pkg() != p.pkg {
				return true
			}
			if v.Pos() < region.Pos() || v.Pos() > region.End() {
				return true
			}
			for _, fl := range lits {
				// declared inside region but outside this literal: captured by it
				if v.Pos() < fl.Pos() || v.Pos() > fl.End() {
					out[v] = true
					if !nonEsc[fl] {
						escapes[v] = true
					}
				}
			}
			return true
		})
	}
	if fl, ok := region.(*ast.FuncLit); ok {
		walk(fl.Body)
	} else {
		walk(region)
	}
	stack := map[types.Object]bool{}
	for o := range out {
		if !addr[o] && !escapes[o] {
			stack[o] = true
		}
	}
	return out, stack, nonEsc
}

// boxDecl declares the box for heap/stack-boxed variable o with initial value val.
func (t *Tx) boxDecl(o types.Object, ctype, val string) string {
	name := cVarName(o.Name())
	if t.stackBox[o] {
		return fmt.Sprintf("%s %s__hv = %s; __typeof__(%s__hv) *%s__h = &%s__hv;", ctype, name, val, name, name, name)
	}
	return fmt.Sprintf("%s *%s__h = %s(%s, %s);", ctype, name, boxFn(o.Type()), ctype, val)
}

func (t *Tx) declObj(o types.Object, ctype, val string) string {
	name := cVarName(o.Name())
	if t.heap[o] {
		return t.boxDecl(o, ctype, val)
	}
	return fmt.Sprintf("%s %s = %s;", ctype, name, val)
}

func (t *Tx) varRef(o types.Object) string {
	if t.heap[o] {
		return "(*" + cVarName(o.Name()) + "__h)"
	}
	return cVarName(o.Name())
}

// heapParams: re-home captured parameters (call at the top of a body).
func (t *Tx) heapParams(sig *types.Signature) {
	if r := sig.Recv(); r != nil && t.heap[r] {
		t.w("%s", t.boxDecl(r, t.ctype(r.Type()), cVarName(r.Name())))
	}
	for i := 0; i < sig.Params().Len(); i++ {
		v := sig.Params().At(i)
		if t.heap[v] {
			t.w("%s", t.boxDecl(v, t.ctype(v.Type()), cVarName(v.Name())))
		}
	}
}

// ---------------------------------------------------------------- defer
// defer f(args): arguments (and the receiver / closure) are evaluated at the
// defer statement into function-scope slots; deferred calls run in reverse
// order before every return.
type deferInfo struct {
	flag string
	call string
}

const deferMarker = "/*@DEFERDECL@*/"

func (t *Tx) deferStmt(s *ast.DeferStmt) {
	c := s.Call
	t.tmp++
	id := fmt.Sprintf("_df%d", t.tmp)
	t.deferDecl = append(t.deferDecl, fmt.Sprintf("bool %s = false;", id))
	ft := t.typeOf(c.Fun)
	sig, _ := ft.(*types.Signature)
	if sig == nil {
		if ft != nil {
			sig, _ = ft.Underlying().(*types.Signature)
		}
	}
	if b, ok := c.Fun.(*ast.Ident); ok {
		if _, isB := t.p.info.Uses[b].(*types.Builtin); isB {
			t.w("/* defer %s(...) not supported */", b.Name)
			return
		}
	}
	if sig == nil {
		t.w("%s;", t.TODO(s, "defer of unknown call"))
		return
	}
	args := t.callArgs(c, sig)
	var temps []string
	for i, a := range args {
		ty := "GoIface"
		if i < sig.Params().Len() {
			ty = t.ctype(sig.Params().At(i).Type())
		}
		tn := fmt.Sprintf("%s_a%d", id, i)
		t.deferDecl = append(t.deferDecl, fmt.Sprintf("%s %s;", ty, tn))
		t.w("%s = %s;", tn, a)
		temps = append(temps, tn)
	}
	var call string
	fn := t.calleeFunc(c)
	_, isLit := c.Fun.(*ast.FuncLit)
	se, isSel := c.Fun.(*ast.SelectorExpr)
	switch {
	case fn != nil && !isSel:
		call = fmt.Sprintf("%s(%s)", cFuncName(t.p, fn), strings.Join(temps, ", "))
		if fn.Pkg() != t.p.pkg {
			externs[fn] = true
		}
	case fn != nil && isSel && t.p.info.Selections[se] == nil:
		// package-qualified function
		call = fmt.Sprintf("%s(%s)", cFuncName(t.p, fn), strings.Join(temps, ", "))
	default:
		// closure, method value or func-typed value: keep it as a GoFunc
		fv := id + "_f"
		t.deferDecl = append(t.deferDecl, fmt.Sprintf("GoFunc %s;", fv))
		if isSel && t.p.info.Selections[se] != nil && t.p.info.Selections[se].Kind() == types.MethodVal {
			t.w("%s = %s;", fv, t.methodValue(se, t.p.info.Selections[se]))
		} else {
			_ = isLit
			t.w("%s = %s;", fv, t.expr(c.Fun))
		}
		ret := t.retTypeOf(sig.Results(), "")
		ptypes := []string{"void *"}
		for i := 0; i < sig.Params().Len(); i++ {
			ptypes = append(ptypes, t.ctype(sig.Params().At(i).Type()))
		}
		call = fmt.Sprintf("((%s (*)(%s))%s.fn)(%s.env%s)", ret, strings.Join(ptypes, ", "), fv, fv, prependComma(temps))
	}
	t.w("%s = true;", id)
	t.defers = append(t.defers, deferInfo{flag: id, call: call})
}

// runDefers emits the deferred calls registered so far (in reverse).
func (t *Tx) runDefers() {
	for i := len(t.defers) - 1; i >= 0; i-- {
		d := t.defers[i]
		t.w("if (%s) { %s = false; %s; }", d.flag, d.flag, d.call)
	}
}

func (t *Tx) finishDefers(body string) string {
	return strings.Replace(body, deferMarker, strings.Join(t.deferDecl, " "), 1)
}

// funcUsesDefer reports whether body contains a defer outside nested literals.
func funcUsesDefer(body ast.Node) bool {
	found := false
	ast.Inspect(body, func(n ast.Node) bool {
		switch n.(type) {
		case *ast.FuncLit:
			return false
		case *ast.DeferStmt:
			found = true
		}
		return !found
	})
	return found
}
