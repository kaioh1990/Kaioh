package main

import (
	"fmt"
	"go/ast"
	"go/importer"
	"go/parser"
	"go/token"
	"go/types"
	"os"
	"path/filepath"
	"strings"
)

func main() {
	dir := os.Args[1]
	fset := token.NewFileSet()
	var files []*ast.File
	ms, _ := filepath.Glob(filepath.Join(dir, "*.go"))
	for _, m := range ms {
		b := filepath.Base(m)
		if strings.HasSuffix(b, "_test.go") || strings.Contains(b, "_vk") || strings.Contains(b, "gl33") || strings.Contains(b, "android") || strings.Contains(b, "ios") || strings.Contains(b, "_other") {
			continue
		}
		f, err := parser.ParseFile(fset, m, nil, 0)
		if err == nil {
			files = append(files, f)
		}
	}
	info := &types.Info{Types: map[ast.Expr]types.TypeAndValue{}}
	conf := types.Config{Importer: importer.ForCompiler(fset, "source", nil), Error: func(error) {}, FakeImportC: true}
	conf.Check("main", fset, files, info)
	for _, f := range files {
		ast.Inspect(f, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok || len(call.Args) != 1 {
				return true
			}
			tv, ok := info.Types[call.Fun]
			if !ok || !tv.IsType() {
				return true
			}
			b, ok := tv.Type.Underlying().(*types.Basic)
			if !ok || b.Info()&types.IsUnsigned == 0 {
				return true
			}
			at, ok := info.Types[call.Args[0]]
			if !ok || at.Value != nil {
				return true
			}
			ab, ok := at.Type.Underlying().(*types.Basic)
			if !ok || ab.Info()&types.IsFloat == 0 {
				return true
			}
			fmt.Printf("%v: %s(%s)\n", fset.Position(call.Pos()), types.TypeString(tv.Type, nil), types.ExprString(call.Args[0]))
			return true
		})
	}
}
