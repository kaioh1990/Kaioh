package main

import (
	"go/ast"
	"go/types"
)

// reach returns the keys of all package functions reachable from roots.
// Interface method calls conservatively reach every method of that name.
// excludedFile: files whose functions are never translated (rendering,
// menus, platform, Vita-only performance code); calls into them become
// stubs.
var excludedFiles = map[string]bool{}

func isExcluded(p *Prog, k string) bool {
	fd := p.funcs[k]
	if fd == nil {
		return true
	}
	return excludedFiles[shortFile(p.fset.Position(fd.Pos()).Filename)]
}

func reach(p *Prog, roots []string) map[string]bool {
	byName := map[string][]string{} // method name -> keys
	for k := range p.funcs {
		fd := p.funcs[k]
		if fd.Recv != nil {
			byName[fd.Name.Name] = append(byName[fd.Name.Name], k)
		}
	}
	keyOf := func(fn *types.Func) string {
		sig := fn.Type().(*types.Signature)
		if r := sig.Recv(); r != nil {
			rt := r.Type()
			if pt, ok := rt.(*types.Pointer); ok {
				rt = pt.Elem()
			}
			if n, ok := rt.(*types.Named); ok {
				return n.Obj().Name() + "." + fn.Name()
			}
			return "" // interface method
		}
		return fn.Name()
	}
	seen := map[string]bool{}
	queue := append([]string{}, roots...)
	for len(queue) > 0 {
		k := queue[0]
		queue = queue[1:]
		if seen[k] {
			continue
		}
		fd := p.funcs[k]
		if fd == nil {
			continue
		}
		seen[k] = true
		if isExcluded(p, k) {
			continue // stub: do not follow its calls
		}
		ast.Inspect(fd, func(n ast.Node) bool {
			switch x := n.(type) {
			case *ast.Ident:
				if fn, ok := p.info.Uses[x].(*types.Func); ok && fn.Pkg() == p.pkg {
					if kk := keyOf(fn); kk != "" {
						queue = append(queue, kk)
					} else {
						queue = append(queue, byName[fn.Name()]...)
					}
				}
			case *ast.SelectorExpr:
				if sel := p.info.Selections[x]; sel != nil {
					if fn, ok := sel.Obj().(*types.Func); ok && fn.Pkg() == p.pkg {
						if _, isIface := sel.Recv().Underlying().(*types.Interface); isIface {
							queue = append(queue, byName[fn.Name()]...)
						} else if kk := keyOf(fn); kk != "" {
							queue = append(queue, kk)
						}
					}
				}
			}
			return true
		})
	}
	return seen
}
