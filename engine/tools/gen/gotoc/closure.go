package main

import (
	"fmt"
	"go/ast"
	"go/types"
	"sort"
	"strings"
)

// Lifted closures: every func literal becomes a static C function taking
// its environment (pointers to captured variables) as the first argument.
var lifted []string
var usedGlobals = map[*types.Var]bool{}
var liftN int

func (t *Tx) funcLit(e *ast.FuncLit) string {
	liftN++
	name := fmt.Sprintf("lambda_%d", liftN)
	sig := t.typeOf(e).(*types.Signature)
	// captured variables: identifiers used inside that resolve to local
	// vars declared outside the literal.
	caps := map[types.Object]bool{}
	ast.Inspect(e.Body, func(n ast.Node) bool {
		id, ok := n.(*ast.Ident)
		if !ok {
			return true
		}
		v, ok := t.p.info.Uses[id].(*types.Var)
		if !ok || v.Parent() == t.p.pkg.Scope() || v.IsField() || v.Pkg() != t.p.pkg {
			return true
		}
		if v.Pos() < e.Pos() || v.Pos() > e.End() {
			caps[v] = true
		}
		return true
	})
	var capList []types.Object
	for o := range caps {
		capList = append(capList, o)
	}
	sort.Slice(capList, func(i, j int) bool { return capList[i].Pos() < capList[j].Pos() })

	envType := "Env_" + name
	var fields []string
	for _, o := range capList {
		fields = append(fields, fmt.Sprintf("%s *%s;", cbase(o.Type()), cVarName(o.Name())))
	}
	if len(fields) == 0 {
		fields = append(fields, "char _unused;")
	}
	lifted = append(lifted, fmt.Sprintf("typedef struct %s { %s } %s;", envType, strings.Join(fields, " "), envType))

	// body
	sub := &Tx{p: t.p, captured: map[types.Object]bool{}, outerFn: t.outerFn, labels: map[string]bool{}}
	for o := range t.captured { // captured through an outer closure: keep pointer chain simple
		_ = o
	}
	for _, o := range capList {
		sub.captured[o] = true
	}
	ret := sub.retTypeOf(sig.Results(), name)
	sub.results = sig.Results()
	sub.retType = ret
	var params []string
	params = append(params, envType+" *_env")
	for i := 0; i < sig.Params().Len(); i++ {
		v := sig.Params().At(i)
		pn := cVarName(v.Name())
		if pn == "" || pn == "_" {
			pn = fmt.Sprintf("_p%d", i)
		}
		params = append(params, cbase(v.Type())+" "+pn)
	}
	var body strings.Builder
	sub.out = new(bytesBuffer)
	sub.w("static %s %s(%s) {", ret, name, strings.Join(params, ", "))
	sub.ind++
	sub.heap = heapVarsOf(t.p, e)
	if funcUsesDefer(e.Body) {
		sub.w("%s", deferMarker)
	}
	sub.heapParams(sig)
	for i := 0; i < sig.Results().Len(); i++ {
		v := sig.Results().At(i)
		if v.Name() != "" && v.Name() != "_" {
			sub.w("%s", sub.declObj(v, cbase(v.Type()), zeroValue(v.Type())))
			sub.namedRes = append(sub.namedRes, sub.varRef(v))
		}
	}
	for _, st := range e.Body.List {
		sub.stmt(st)
	}
	sub.runDefers()
	if len(sub.namedRes) > 0 {
		sub.w("%s", sub.returnNamed())
	}
	sub.ind--
	sub.w("}")
	body.WriteString(sub.finishDefers(sub.out.String()))
	t.todo += sub.todo
	lifted = append(lifted, body.String())

	// environment value at the creation site: pointers to the captured
	// variables (valid while the enclosing function runs).
	var inits []string
	for _, o := range capList {
		if t.captured[o] {
			inits = append(inits, "_env->"+cVarName(o.Name()))
		} else if t.heap[o] {
			inits = append(inits, cVarName(o.Name())+"__h")
		} else {
			inits = append(inits, "&"+cVarName(o.Name()))
		}
	}
	if len(inits) == 0 {
		inits = append(inits, "0")
	}
	return fmt.Sprintf("((GoFunc){(void *)%s, go_box(%s, ((%s){%s}))})", name, envType, envType, strings.Join(inits, ", "))
}

// Call through a func value: (Ret(*)(void *, P...))f.fn applied to f.env.
func (t *Tx) indirectCall(c *ast.CallExpr) string {
	ft := t.typeOf(c.Fun)
	if ft == nil {
		return t.TODO(c, "call of unknown function")
	}
	sig, ok := ft.Underlying().(*types.Signature)
	if !ok {
		return t.TODO(c, "indirect call: not a func")
	}
	ret := t.retTypeOf(sig.Results(), fmt.Sprintf("fnval_%p", sig))
	ptypes := []string{"void *"}
	for i := 0; i < sig.Params().Len(); i++ {
		ptypes = append(ptypes, cbase(sig.Params().At(i).Type()))
	}
	args := t.callArgs(c, sig)
	f := t.newTmpExpr(c.Fun)
	return fmt.Sprintf("((%s (*)(%s))%s.fn)(%s.env%s)", ret, strings.Join(ptypes, ", "), f, f, prependComma(args))
}

func (t *Tx) newTmpExpr(e ast.Expr) string {
	return "(" + t.expr(e) + ")"
}
