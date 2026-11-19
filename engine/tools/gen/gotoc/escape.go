package main

import (
	"go/ast"
	"go/token"
	"go/types"
)

// ---------------------------------------------------------------- escape lite
// A function literal is non-escaping when it can only run while the
// enclosing function body (region) is executing and the block that created
// it is still live:
//   - it is called directly: func(){...}(), or
//   - it initialises a local (f := func..., var f = func...) whose every use
//     is in call position f(...),
// and no such call sits in a defer/go statement or inside another literal.
// Its environment can then live on the C stack, and variables captured only
// by non-escaping literals need no heap box.
func nonEscapingLits(p *Prog, region ast.Node) map[*ast.FuncLit]bool {
	out := map[*ast.FuncLit]bool{}
	parent := map[ast.Node]ast.Node{}
	var stack []ast.Node
	ast.Inspect(region, func(n ast.Node) bool {
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
	up := func(n ast.Node) ast.Node {
		q := parent[n]
		for {
			pe, ok := q.(*ast.ParenExpr)
			if !ok {
				return q
			}
			q = parent[pe]
		}
	}
	// a call is "plain" when it is not deferred/spawned and not inside a
	// nested literal of the region
	plainCall := func(c *ast.CallExpr) bool {
		if s, ok := parent[c].(*ast.DeferStmt); ok && s.Call == c {
			return false
		}
		if s, ok := parent[c].(*ast.GoStmt); ok && s.Call == c {
			return false
		}
		for q := parent[ast.Node(c)]; q != nil; q = parent[q] {
			if q == region {
				return true
			}
			if _, ok := q.(*ast.FuncLit); ok {
				return false
			}
		}
		return true
	}
	isCallFun := func(n ast.Node) (*ast.CallExpr, bool) {
		c, ok := up(n).(*ast.CallExpr)
		if !ok {
			return nil, false
		}
		f := c.Fun
		for {
			pe, ok := f.(*ast.ParenExpr)
			if !ok {
				break
			}
			f = pe.X
		}
		return c, f == n
	}
	// uses of each local, for the assigned-literal case
	uses := map[types.Object][]*ast.Ident{}
	ast.Inspect(region, func(n ast.Node) bool {
		if id, ok := n.(*ast.Ident); ok {
			if o := p.info.Uses[id]; o != nil {
				uses[o] = append(uses[o], id)
			}
		}
		return true
	})
	onlyCalled := func(o types.Object) bool {
		if o == nil || len(uses[o]) == 0 {
			return false
		}
		for _, id := range uses[o] {
			c, ok := isCallFun(id)
			if !ok || !plainCall(c) {
				return false
			}
		}
		return true
	}
	ast.Inspect(region, func(n ast.Node) bool {
		fl, ok := n.(*ast.FuncLit)
		if !ok || fl == region {
			return true
		}
		if c, ok := isCallFun(fl); ok {
			if plainCall(c) {
				out[fl] = true
			}
			return true
		}
		switch q := up(fl).(type) {
		case *ast.AssignStmt:
			if q.Tok == token.DEFINE && len(q.Lhs) == 1 && len(q.Rhs) == 1 {
				if id, ok := q.Lhs[0].(*ast.Ident); ok && onlyCalled(p.info.Defs[id]) {
					out[fl] = true
				}
			}
		case *ast.ValueSpec:
			if len(q.Names) == 1 && len(q.Values) == 1 && onlyCalled(p.info.Defs[q.Names[0]]) {
				out[fl] = true
			}
		}
		return true
	})
	return out
}
