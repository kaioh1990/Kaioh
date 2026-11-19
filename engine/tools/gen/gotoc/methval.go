package main

import (
	"fmt"
	"go/ast"
	"go/types"
	"strings"
)

// Method values (x.M as a func value): GoFunc{thunk, receiver}. Pointer
// receivers keep the pointer; value receivers and interfaces get a boxed
// copy (Go evaluates and copies the receiver at the point of the
// method value).
var methodThunks = map[string]string{}

func (t *Tx) methodValue(e *ast.SelectorExpr, sel *types.Selection) string {
	fn := sel.Obj().(*types.Func)
	sig := fn.Type().(*types.Signature)
	recvT := sel.Recv()
	var params, args []string
	for i := 0; i < sig.Params().Len(); i++ {
		params = append(params, fmt.Sprintf("%s _a%d", cbase(sig.Params().At(i).Type()), i))
		args = append(args, fmt.Sprintf("_a%d", i))
	}
	ret := t.retTypeOf(sig.Results(), "")
	retKw := "return "
	if ret == "void" {
		retKw = ""
	}
	if isIface(recvT) {
		disp := ifaceDispatcherName(fn.Name(), sig)
		if _, ok := ifaceMethods[disp]; !ok {
			ifaceMethods[disp] = ifaceMethod{name: fn.Name(), method: fn}
		}
		name := "mv_" + disp
		if _, ok := methodThunks[name]; !ok {
			methodThunks[name] = fmt.Sprintf("static %s %s(void *_env%s) { %s%s(*(GoIface *)_env%s); }",
				ret, name, prependComma(params), retKw, disp, prependComma(args))
		}
		return fmt.Sprintf("((GoFunc){(void *)%s, go_box(GoIface, %s)})", name, t.expr(e.X))
	}
	// promoted methods: walk to the embedded receiver
	recv := t.methodRecv(e.X, sel, sig)
	_, wantPtr := sig.Recv().Type().(*types.Pointer)
	rt := sig.Recv().Type()
	cf := cFuncName(t.p, fn)
	name := "mv_" + cf
	if _, ok := methodThunks[name]; !ok {
		var r string
		if wantPtr {
			r = fmt.Sprintf("(%s)_env", cbase(rt))
		} else {
			r = fmt.Sprintf("*(%s *)_env", cbase(rt))
		}
		methodThunks[name] = fmt.Sprintf("static %s %s(void *_env%s) { %s%s(%s%s); }",
			ret, name, prependComma(params), retKw, cf, r, prependComma(args))
	}
	if wantPtr {
		return fmt.Sprintf("((GoFunc){(void *)%s, (void *)(%s)})", name, recv)
	}
	return fmt.Sprintf("((GoFunc){(void *)%s, go_box(%s, %s)})", name, cbase(rt), recv)
}

func thunkLines() []string {
	var out []string
	for _, v := range methodThunks {
		out = append(out, v)
	}
	sortStrings(out)
	return out
}

var _ = strings.Join
