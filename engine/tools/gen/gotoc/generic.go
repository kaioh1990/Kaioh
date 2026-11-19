package main

import (
	"go/ast"
	"go/types"
	"strings"
)

// Generic functions are monomorphised: each instantiation reached from
// translated code is translated again with its type parameters bound.
var curSubst map[*types.TypeParam]types.Type

type genInst struct {
	fn    *types.Func
	targs []types.Type
	name  string
}

var genInsts = map[string]*genInst{}
var genPending []*genInst

func substT(ty types.Type) types.Type {
	if ty == nil {
		return ty
	}
	ty = types.Unalias(ty)
	if curSubst == nil {
		return ty
	}
	return deepSubst(ty)
}

func deepSubst(ty types.Type) types.Type {
	switch u := ty.(type) {
	case *types.TypeParam:
		if s, ok := curSubst[u]; ok {
			return s
		}
		return u
	case *types.Pointer:
		if e := deepSubst(u.Elem()); e != u.Elem() {
			return types.NewPointer(e)
		}
	case *types.Slice:
		if e := deepSubst(u.Elem()); e != u.Elem() {
			return types.NewSlice(e)
		}
	case *types.Array:
		if e := deepSubst(u.Elem()); e != u.Elem() {
			return types.NewArray(e, u.Len())
		}
	case *types.Map:
		k, v := deepSubst(u.Key()), deepSubst(u.Elem())
		if k != u.Key() || v != u.Elem() {
			return types.NewMap(k, v)
		}
	case *types.Tuple:
		if u == nil {
			return u
		}
		changed := false
		vars := make([]*types.Var, u.Len())
		for i := 0; i < u.Len(); i++ {
			v := u.At(i)
			nt := deepSubst(v.Type())
			if nt != v.Type() {
				changed = true
			}
			vars[i] = types.NewVar(v.Pos(), v.Pkg(), v.Name(), nt)
		}
		if changed {
			return types.NewTuple(vars...)
		}
	case *types.Signature:
		p := deepSubst(u.Params())
		r := deepSubst(u.Results())
		if p != types.Type(u.Params()) || r != types.Type(u.Results()) {
			pt, _ := p.(*types.Tuple)
			rt, _ := r.(*types.Tuple)
			return types.NewSignatureType(u.Recv(), nil, nil, pt, rt, u.Variadic())
		}
	case *types.Named:
		if ta := u.TypeArgs(); ta != nil && ta.Len() > 0 {
			changed := false
			var args []types.Type
			for i := 0; i < ta.Len(); i++ {
				a := deepSubst(ta.At(i))
				if a != ta.At(i) {
					changed = true
				}
				args = append(args, a)
			}
			if changed {
				if inst, err := types.Instantiate(nil, u.Origin(), args, false); err == nil {
					return inst
				}
			}
		}
	}
	return ty
}

// genericCall: name and instantiated signature for a call of a generic
// function (registering the instance for translation).
func (t *Tx) genericCall(c *ast.CallExpr, fn *types.Func) (string, *types.Signature) {
	id := identOf(c.Fun)
	if ix, ok := c.Fun.(*ast.IndexExpr); ok {
		id = identOf(ix.X)
	}
	if ix, ok := c.Fun.(*ast.IndexListExpr); ok {
		id = identOf(ix.X)
	}
	inst, ok := t.p.info.Instances[id]
	if !ok {
		return "", nil
	}
	var targs []types.Type
	var parts []string
	for i := 0; i < inst.TypeArgs.Len(); i++ {
		a := substT(inst.TypeArgs.At(i))
		targs = append(targs, a)
		parts = append(parts, mangle(a))
	}
	name := cFuncName(t.p, fn) + "_" + strings.Join(parts, "_")
	if _, ok := genInsts[name]; !ok && isOwn(fn.Pkg()) {
		gi := &genInst{fn: fn, targs: targs, name: name}
		genInsts[name] = gi
		genPending = append(genPending, gi)
	}
	return name, substT(inst.Type).(*types.Signature)
}

func bindTypeParams(fn *types.Func, targs []types.Type) map[*types.TypeParam]types.Type {
	m := map[*types.TypeParam]types.Type{}
	sig := fn.Type().(*types.Signature)
	tps := sig.TypeParams()
	if rtp := sig.RecvTypeParams(); rtp != nil && rtp.Len() > 0 {
		tps = rtp // method of a generic type: parameters come from the receiver
	}
	for i := 0; i < tps.Len() && i < len(targs); i++ {
		m[tps.At(i)] = targs[i]
	}
	return m
}

// methodCName: C name of a method; methods of instantiated generic types
// (RingBuffer[GameInput].Push) are monomorphised like generic functions.
func methodCName(p *Prog, fn *types.Func, recv types.Type) string {
	base := substT(recv)
	if pt, ok := base.(*types.Pointer); ok {
		base = pt.Elem()
	}
	n, ok := base.(*types.Named)
	if !ok || n.TypeArgs().Len() == 0 || !isOwn(fn.Pkg()) {
		return cFuncName(p, fn)
	}
	orig := fn.Origin()
	var targs []types.Type
	var parts []string
	for i := 0; i < n.TypeArgs().Len(); i++ {
		a := substT(n.TypeArgs().At(i))
		targs = append(targs, a)
		parts = append(parts, mangle(a))
	}
	name := cFuncName(p, orig) + "_" + strings.Join(parts, "_")
	if _, ok := genInsts[name]; !ok {
		gi := &genInst{fn: orig, targs: targs, name: name}
		genInsts[name] = gi
		genPending = append(genPending, gi)
	}
	return name
}

// isGenericMethodDecl: a method declared on a generic type (translated only
// per instantiation).
func isGenericMethodDecl(fd *ast.FuncDecl) bool {
	if fd.Recv == nil || len(fd.Recv.List) == 0 {
		return false
	}
	t := fd.Recv.List[0].Type
	if s, ok := t.(*ast.StarExpr); ok {
		t = s.X
	}
	switch t.(type) {
	case *ast.IndexExpr, *ast.IndexListExpr:
		return true
	}
	return false
}
