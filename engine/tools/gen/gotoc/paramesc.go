package main

import (
	"go/ast"
	"go/token"
	"go/types"
)

// ---------------------------------------------------------------- parameter escape
// A pointer parameter (or pointer receiver) "does not escape" when the
// function only dereferences it: reads or writes through it (*p, p.f, p[i]
// on an array pointer), compares it, or passes it on to parameters that do
// not escape either. Then a caller may pass the address of a local (&v,
// &v.f) without moving v to the heap: the pointer is dead when the call
// returns. Everything else counts as escaping (stored, returned, captured
// by a closure, converted to an interface, sliced, address of a field
// taken, passed to a function value, an interface method, a builtin, a
// deferred or spawned call, a function without a translated body).
//
// Computed once for the whole program as a greatest fixpoint: every
// candidate starts as non-escaping and is cleared by any escaping use
// until nothing changes (so mutually recursive pass-throughs stay
// non-escaping). The result only decides heap versus stack storage of
// address-taken locals (heapVarsOf), never what the program computes.

type escParam struct {
	fn  *types.Func
	idx int // parameter index, -1 for the receiver
}

var paramNoEsc map[escParam]bool

type escFunc struct {
	fn     *types.Func
	fd     *ast.FuncDecl
	parent map[ast.Node]ast.Node
	params map[*types.Var]int
}

func computeParamEscape(p *Prog, translated func(k string) bool) {
	paramNoEsc = map[escParam]bool{}
	var fns []*escFunc
	for k, fd := range p.funcs {
		if fd.Body == nil || fd.Type.TypeParams != nil || isGenericMethodDecl(fd) || !translated(k) {
			continue
		}
		fn, ok := p.info.Defs[fd.Name].(*types.Func)
		if !ok {
			continue
		}
		sig := fn.Type().(*types.Signature)
		ef := &escFunc{fn: fn, fd: fd, params: map[*types.Var]int{}}
		if r := sig.Recv(); r != nil && isPtrType(r.Type()) {
			ef.params[r] = -1
		}
		for i := 0; i < sig.Params().Len(); i++ {
			if sig.Variadic() && i == sig.Params().Len()-1 {
				break
			}
			if v := sig.Params().At(i); isPtrType(v.Type()) {
				ef.params[v] = i
			}
		}
		if len(ef.params) == 0 {
			continue
		}
		ef.parent = parentMap(fd.Body)
		for _, i := range ef.params {
			paramNoEsc[escParam{fn, i}] = true
		}
		fns = append(fns, ef)
	}
	for changed := true; changed; {
		changed = false
		for _, ef := range fns {
			for v, i := range ef.params {
				k := escParam{ef.fn, i}
				if paramNoEsc[k] && ef.escapes(p, v) {
					paramNoEsc[k] = false
					changed = true
				}
			}
		}
	}
}

func isPtrType(t types.Type) bool {
	_, ok := t.Underlying().(*types.Pointer)
	return ok
}

func parentMap(root ast.Node) map[ast.Node]ast.Node {
	parent := map[ast.Node]ast.Node{}
	var stack []ast.Node
	ast.Inspect(root, func(n ast.Node) bool {
		if n == nil {
			stack = stack[:len(stack)-1]
			return true
		}
		if len(stack) > 0 {
			parent[n] = stack[len(stack)-1]
		}
		stack = append(stack, n)
		return true
	})
	return parent
}

func upParen(parent map[ast.Node]ast.Node, n ast.Node) (ast.Node, ast.Node) {
	q := parent[n]
	for {
		pe, ok := q.(*ast.ParenExpr)
		if !ok {
			return q, n
		}
		n = pe
		q = parent[pe]
	}
}

// escapes reports whether parameter v of ef has an escaping use.
func (ef *escFunc) escapes(p *Prog, v *types.Var) bool {
	esc := false
	ast.Inspect(ef.fd.Body, func(n ast.Node) bool {
		if esc {
			return false
		}
		id, ok := n.(*ast.Ident)
		if !ok || p.info.Uses[id] != v {
			return true
		}
		if !ef.useOK(p, id) {
			esc = true
		}
		return true
	})
	return esc
}

func (ef *escFunc) inLit(n ast.Node) bool {
	for q := ef.parent[n]; q != nil; q = ef.parent[q] {
		if _, ok := q.(*ast.FuncLit); ok {
			return true
		}
	}
	return false
}

// useOK: the use id of a pointer parameter does not let the pointer escape.
func (ef *escFunc) useOK(p *Prog, id *ast.Ident) bool {
	if ef.inLit(id) {
		return false
	}
	q, n := upParen(ef.parent, id)
	switch q := q.(type) {
	case *ast.StarExpr: // *p: the pointee as a value
		return ef.derefOK(p, q)
	case *ast.BinaryExpr:
		return q.Op == token.EQL || q.Op == token.NEQ
	case *ast.SelectorExpr:
		if q.X != n {
			return false
		}
		sel := p.info.Selections[q]
		if sel == nil {
			return false
		}
		switch sel.Kind() {
		case types.FieldVal:
			return ef.derefOK(p, q)
		case types.MethodVal:
			return ef.recvOK(p, q, sel)
		}
		return false
	case *ast.IndexExpr: // p[i] on an array pointer
		if q.X != n {
			return false
		}
		if _, ok := p.info.TypeOf(q.X).Underlying().(*types.Pointer); !ok {
			return false
		}
		return ef.derefOK(p, q)
	case *ast.CallExpr:
		return ef.argOK(p, q, n)
	}
	return false
}

// derefOK: e denotes storage reached through the parameter (*p, p.f, p[i],
// and further field / array element selections of those); using it as a
// value or assigning to it is fine, taking its address or slicing it is not.
func (ef *escFunc) derefOK(p *Prog, e ast.Expr) bool {
	for {
		q, n := upParen(ef.parent, e)
		switch q := q.(type) {
		case *ast.UnaryExpr:
			if q.Op == token.AND {
				return false
			}
			return true
		case *ast.SliceExpr:
			if q.X == n {
				return false // a slice of an array reached through p
			}
			return true
		case *ast.SelectorExpr:
			if q.X != n {
				return true
			}
			sel := p.info.Selections[q]
			if sel == nil {
				return false
			}
			if sel.Kind() == types.MethodVal {
				// a method on the pointee: a pointer receiver takes its address
				if isPtrType(sel.Obj().(*types.Func).Type().(*types.Signature).Recv().Type()) {
					return false
				}
				if _, isIface := sel.Recv().Underlying().(*types.Interface); isIface {
					return true // the interface value is copied
				}
				return true
			}
			if isPtrType(p.info.TypeOf(n.(ast.Expr))) {
				return true // p.f where f is itself a pointer: a loaded value
			}
			e = q
			continue
		case *ast.IndexExpr:
			if q.X != n {
				return true
			}
			if _, isArr := p.info.TypeOf(n.(ast.Expr)).Underlying().(*types.Array); isArr {
				e = q
				continue
			}
			return true // slice or map element: the value is loaded first
		}
		return true
	}
}

// recvOK: p.M(...) with p a pointer parameter.
func (ef *escFunc) recvOK(p *Prog, q *ast.SelectorExpr, sel *types.Selection) bool {
	c, ok := ef.parent[q].(*ast.CallExpr)
	if !ok || c.Fun != q || ef.deferOrGo(c) {
		return false // method value
	}
	fn, ok := sel.Obj().(*types.Func)
	if !ok {
		return false
	}
	if _, isIface := sel.Recv().Underlying().(*types.Interface); isIface {
		return false
	}
	recv := fn.Type().(*types.Signature).Recv()
	if recv == nil {
		return false
	}
	if !isPtrType(recv.Type()) {
		return true // value receiver: *p is copied
	}
	if len(sel.Index()) > 1 {
		return false // promoted through embedding: an interior pointer
	}
	return paramNoEsc[escParam{fn.Origin(), -1}]
}

// argOK: n is (part of) a call expression c with the parameter.
func (ef *escFunc) argOK(p *Prog, c *ast.CallExpr, n ast.Node) bool {
	if c.Fun == n || ef.deferOrGo(c) {
		return false
	}
	j := -1
	for i, a := range c.Args {
		if a == n {
			j = i
		}
	}
	if j < 0 || c.Ellipsis.IsValid() {
		return false
	}
	return argNoEsc(p, c, j)
}

func (ef *escFunc) deferOrGo(c *ast.CallExpr) bool {
	switch s := ef.parent[c].(type) {
	case *ast.DeferStmt:
		return s.Call == c
	case *ast.GoStmt:
		return s.Call == c
	}
	return false
}

// argNoEsc: argument j of the static call c goes to a parameter that does
// not escape.
func argNoEsc(p *Prog, c *ast.CallExpr, j int) bool {
	fun := c.Fun
	for {
		pe, ok := fun.(*ast.ParenExpr)
		if !ok {
			break
		}
		fun = pe.X
	}
	var fn *types.Func
	switch f := fun.(type) {
	case *ast.Ident:
		fn, _ = p.info.Uses[f].(*types.Func)
	case *ast.SelectorExpr:
		if sel := p.info.Selections[f]; sel != nil {
			if sel.Kind() != types.MethodVal {
				return false
			}
			if _, isIface := sel.Recv().Underlying().(*types.Interface); isIface {
				return false
			}
			fn, _ = sel.Obj().(*types.Func)
		} else {
			fn, _ = p.info.Uses[f.Sel].(*types.Func) // pkg.Func
		}
	}
	if fn == nil {
		return false
	}
	sig, ok := p.info.TypeOf(fun).(*types.Signature)
	if !ok || (sig.Variadic() && j >= sig.Params().Len()-1) {
		return false
	}
	return paramNoEsc[escParam{fn.Origin(), j}]
}

// addrArgNoEsc: the address-of expression u (&v, &v.f, &v[i]) is a direct
// argument of a call whose parameter does not escape.
func addrArgNoEsc(p *Prog, parent map[ast.Node]ast.Node, u *ast.UnaryExpr) bool {
	if paramNoEsc == nil {
		return false
	}
	q, n := upParen(parent, u)
	c, ok := q.(*ast.CallExpr)
	if !ok || c.Fun == n || c.Ellipsis.IsValid() {
		return false
	}
	switch s := parent[c].(type) {
	case *ast.DeferStmt:
		if s.Call == c {
			return false
		}
	case *ast.GoStmt:
		if s.Call == c {
			return false
		}
	}
	for j, a := range c.Args {
		if a == n {
			return argNoEsc(p, c, j)
		}
	}
	return false
}

// ---------------------------------------------------------------- capture by value
// A local that is never modified after its declaration (no assignment,
// ++/--, range assignment, &v, and no pointer-receiver method call or
// assignment through a value path v.f / v[i] on an array) can be captured
// by value: the closure's environment holds a copy instead of a pointer, and
// the variable needs no heap box. The copy is taken when the literal is
// evaluated; the variable already holds its final value then, because a
// variable is only captured after its declaration and is never written
// afterwards.
var mutatedVars map[*types.Var]bool

func computeMutated(p *Prog) {
	mutatedVars = map[*types.Var]bool{}
	mark := func(e ast.Expr) {
		if v := rootVar(p, e); v != nil {
			mutatedVars[v] = true
		}
	}
	for _, f := range p.files {
		ast.Inspect(f, func(n ast.Node) bool {
			switch s := n.(type) {
			case *ast.AssignStmt:
				for _, l := range s.Lhs {
					if id, ok := l.(*ast.Ident); ok && s.Tok == token.DEFINE && p.info.Defs[id] != nil {
						continue // a definition, not an assignment
					}
					mark(l)
				}
			case *ast.IncDecStmt:
				mark(s.X)
			case *ast.RangeStmt:
				if s.Tok == token.ASSIGN {
					if s.Key != nil {
						mark(s.Key)
					}
					if s.Value != nil {
						mark(s.Value)
					}
				}
			case *ast.UnaryExpr:
				if s.Op == token.AND {
					mark(s.X)
				}
			case *ast.SelectorExpr:
				// v.M / v.M() with a pointer receiver takes &v implicitly
				if sel := p.info.Selections[s]; sel != nil && sel.Kind() == types.MethodVal {
					if fn, ok := sel.Obj().(*types.Func); ok {
						if r := fn.Type().(*types.Signature).Recv(); r != nil && isPtrType(r.Type()) && !isPtrType(p.info.TypeOf(s.X)) {
							mark(s.X)
						}
					}
					if _, isIface := sel.Recv().Underlying().(*types.Interface); !isIface && len(sel.Index()) > 1 {
						mark(s.X) // promoted method: conservatively a mutation
					}
				}
			}
			return true
		})
	}
}

// rootVar: the local variable whose own storage e denotes (v, v.f, v[i] on
// arrays, through parentheses), or nil.
func rootVar(p *Prog, e ast.Expr) *types.Var {
	for {
		switch x := e.(type) {
		case *ast.ParenExpr:
			e = x.X
			continue
		case *ast.Ident:
			if v, ok := p.info.Uses[x].(*types.Var); ok {
				return v
			}
			if v, ok := p.info.Defs[x].(*types.Var); ok {
				return v
			}
			return nil
		case *ast.SelectorExpr:
			if sel := p.info.Selections[x]; sel != nil && sel.Kind() == types.FieldVal && !isPtrType(p.info.TypeOf(x.X)) {
				e = x.X
				continue
			}
			return nil
		case *ast.IndexExpr:
			if _, isArr := p.info.TypeOf(x.X).Underlying().(*types.Array); isArr {
				e = x.X
				continue
			}
			return nil
		}
		return nil
	}
}

// byValue: captured variables of this kind are copied into the closure.
func byValue(o types.Object) bool {
	v, ok := o.(*types.Var)
	return ok && mutatedVars != nil && !mutatedVars[v]
}
