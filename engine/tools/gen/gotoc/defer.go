package main

import (
	"fmt"
	"go/ast"
	"go/types"
	"strings"
)

// ---------------------------------------------------------------- heap vars
// Locals captured by closures are heap-allocated (Go's escape analysis does
// the same), so closures that outlive their function stay valid.
func heapVarsOf(p *Prog, region ast.Node) map[types.Object]bool {
	out := map[types.Object]bool{}
	ast.Inspect(region, func(n ast.Node) bool {
		fl, ok := n.(*ast.FuncLit)
		if !ok || fl == region {
			return true
		}
		ast.Inspect(fl.Body, func(n2 ast.Node) bool {
			id, ok := n2.(*ast.Ident)
			if !ok {
				return true
			}
			v, ok := p.info.Uses[id].(*types.Var)
			if !ok || v.IsField() || v.Parent() == p.pkg.Scope() || v.Pkg() != p.pkg {
				return true
			}
			// declared inside region but outside this literal
			if v.Pos() >= region.Pos() && v.Pos() <= region.End() && (v.Pos() < fl.Pos() || v.Pos() > fl.End()) {
				out[v] = true
			}
			return true
		})
		return true
	})
	return out
}

func (t *Tx) declObj(o types.Object, ctype, val string) string {
	name := cVarName(o.Name())
	if t.heap[o] {
		return fmt.Sprintf("%s *%s__h = go_box(%s, %s);", ctype, name, ctype, val)
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
		t.w("%s *%s__h = go_box(%s, %s);", t.ctype(r.Type()), cVarName(r.Name()), t.ctype(r.Type()), cVarName(r.Name()))
	}
	for i := 0; i < sig.Params().Len(); i++ {
		v := sig.Params().At(i)
		if t.heap[v] {
			t.w("%s *%s__h = go_box(%s, %s);", t.ctype(v.Type()), cVarName(v.Name()), t.ctype(v.Type()), cVarName(v.Name()))
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
