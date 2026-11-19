package main

import (
	"fmt"
	"go/ast"
	"go/token"
	"go/types"
	"strings"
)

// Channels, select and `go` map onto the cooperative goroutine runtime
// (src/core/sched.c): GoChan*, go_chan_*, go_select, go_spawn.

func chanElem(t types.Type) types.Type {
	if c, ok := t.Underlying().(*types.Chan); ok {
		return c.Elem()
	}
	return types.Typ[types.Int]
}

// <-ch as an expression
func (t *Tx) recvExpr(e *ast.UnaryExpr) string {
	el := chanElem(t.typeOf(e.X))
	tv := t.newTmp()
	return fmt.Sprintf("({ %s %s; go_chan_recv(%s, &%s); %s; })", t.ctype(el), tv, t.expr(e.X), tv, tv)
}

func (t *Tx) sendStmt(s *ast.SendStmt) {
	el := chanElem(t.typeOf(s.Chan))
	tv := t.newTmp()
	t.w("{ %s %s = %s; go_chan_send(%s, &%s); }", t.ctype(el), tv, t.exprConv(s.Value, el), t.expr(s.Chan), tv)
}

func (t *Tx) selectStmt(s *ast.SelectStmt) {
	var cases []*ast.CommClause
	var deflt *ast.CommClause
	for _, cc := range s.Body.List {
		c := cc.(*ast.CommClause)
		if c.Comm == nil {
			deflt = c
		} else {
			cases = append(cases, c)
		}
	}
	t.w("{")
	t.ind++
	sc := t.newTmp()
	if len(cases) > 0 {
		t.w("GoSelCase %s[%d];", sc, len(cases))
	}
	type slot struct{ val, ok string }
	slots := make([]slot, len(cases))
	for i, c := range cases {
		switch cm := c.Comm.(type) {
		case *ast.SendStmt:
			el := chanElem(t.typeOf(cm.Chan))
			v := t.newTmp()
			t.w("%s %s = %s;", t.ctype(el), v, t.exprConv(cm.Value, el))
			t.w("%s[%d] = (GoSelCase){%s, 1, &%s, NULL};", sc, i, t.expr(cm.Chan), v)
		default:
			var rx *ast.UnaryExpr
			switch r := cm.(type) {
			case *ast.ExprStmt:
				rx, _ = r.X.(*ast.UnaryExpr)
			case *ast.AssignStmt:
				rx, _ = r.Rhs[0].(*ast.UnaryExpr)
			}
			if rx == nil {
				t.w("%s;", t.TODO(c, "select case"))
				continue
			}
			el := chanElem(t.typeOf(rx.X))
			v, ok := t.newTmp(), t.newTmp()
			t.w("%s %s; bool %s = false;", t.ctype(el), v, ok)
			t.w("%s[%d] = (GoSelCase){%s, 2, &%s, &%s};", sc, i, t.expr(rx.X), v, ok)
			slots[i] = slot{v, ok}
		}
	}
	k := t.newTmp()
	arr := "NULL"
	if len(cases) > 0 {
		arr = sc
	}
	t.w("int %s = go_select(%s, %d, %v);", k, arr, len(cases), deflt != nil)
	t.w("do {")
	t.ind++
	t.inSwitch++
	for i, c := range cases {
		kw := "if"
		if i > 0 {
			kw = "} else if"
		}
		t.w("%s (%s == %d) {", kw, k, i)
		t.ind++
		if as, ok := c.Comm.(*ast.AssignStmt); ok {
			el := chanElem(t.typeOf(as.Rhs[0].(*ast.UnaryExpr).X))
			t.defOrSet(as.Lhs[0], as.Tok, slots[i].val, el)
			if len(as.Lhs) > 1 {
				t.defOrSet(as.Lhs[1], as.Tok, slots[i].ok, types.Typ[types.Bool])
			}
		}
		for _, st := range c.Body {
			t.stmt(st)
		}
		t.ind--
	}
	if deflt != nil {
		if len(cases) > 0 {
			t.w("} else {")
		} else {
			t.w("{")
		}
		t.ind++
		for _, st := range deflt.Body {
			t.stmt(st)
		}
		t.ind--
	}
	if len(cases) > 0 || deflt != nil {
		t.w("}")
	}
	t.inSwitch--
	t.ind--
	t.w("} while (0);")
	t.ind--
	t.w("}")
}

var goThunkN int

// go f(args): arguments evaluated now into a heap environment; a lifted
// thunk makes the call when the scheduler runs the new goroutine.
func (t *Tx) goStmt(s *ast.GoStmt) {
	c := s.Call
	ft := t.typeOf(c.Fun)
	var sig *types.Signature
	if ft != nil {
		sig, _ = ft.Underlying().(*types.Signature)
	}
	if sig == nil {
		t.w("%s;", t.TODO(s, "go of unknown call"))
		return
	}
	var fv string
	if se, ok := c.Fun.(*ast.SelectorExpr); ok && t.p.info.Selections[se] != nil && t.p.info.Selections[se].Kind() == types.MethodVal {
		fv = t.methodValue(se, t.p.info.Selections[se])
	} else if fn := t.calleeFunc(c); fn != nil {
		fv = t.funcValue(fn)
	} else {
		fv = t.expr(c.Fun)
	}
	args := t.callArgs(c, sig)
	goThunkN++
	env := fmt.Sprintf("GoEnv_%d", goThunkN)
	thunk := fmt.Sprintf("go_thunk_%d", goThunkN)
	fields := []string{"GoFunc f;"}
	var ptypes, callArgs []string
	ptypes = append(ptypes, "void *")
	for i := range args {
		ty := "GoIface"
		if i < sig.Params().Len() {
			ty = t.ctype(sig.Params().At(i).Type())
		}
		fields = append(fields, fmt.Sprintf("%s a%d;", ty, i))
		ptypes = append(ptypes, ty)
		callArgs = append(callArgs, fmt.Sprintf("e->a%d", i))
	}
	ret := t.retTypeOf(sig.Results(), "")
	lifted = append(lifted, fmt.Sprintf("typedef struct %s { %s } %s;\nstatic void %s(void *e_) { %s *e = (%s *)e_; ((%s (*)(%s))e->f.fn)(e->f.env%s); }",
		env, strings.Join(fields, " "), env, thunk, env, env, ret, strings.Join(ptypes, ", "), prependComma(callArgs)))
	ge := t.newTmp()
	t.w("{")
	t.ind++
	t.w("%s *%s = (%s *)gc_alloc(sizeof(%s));", env, ge, env, env)
	t.w("%s->f = %s;", ge, fv)
	for i, a := range args {
		t.w("%s->a%d = %s;", ge, i, a)
	}
	t.w("go_spawn(%s, %s);", thunk, ge)
	t.ind--
	t.w("}")
}

var _ = token.ARROW
