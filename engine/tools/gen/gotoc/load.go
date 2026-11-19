package main

import (
	"go/ast"
	"go/build"
	"go/importer"
	"go/parser"
	"go/token"
	"go/types"
	"path/filepath"
)

type Prog struct {
	fset  *token.FileSet
	files []*ast.File
	pkg   *types.Package
	info  *types.Info
	funcs map[string]*ast.FuncDecl // "Recv.Name" or "Name"
}

func load(dir string) *Prog {
	ctx := build.Default
	ctx.BuildTags = []string{"vita"}
	bp, _ := ctx.ImportDir(dir, 0)
	p := &Prog{fset: token.NewFileSet(), funcs: map[string]*ast.FuncDecl{}}
	for _, f := range append(bp.GoFiles, bp.CgoFiles...) {
		af, err := parser.ParseFile(p.fset, filepath.Join(dir, f), nil, parser.ParseComments)
		if err == nil {
			p.files = append(p.files, af)
		}
	}
	p.info = &types.Info{
		Types:      map[ast.Expr]types.TypeAndValue{},
		Defs:       map[*ast.Ident]types.Object{},
		Uses:       map[*ast.Ident]types.Object{},
		Selections: map[*ast.SelectorExpr]*types.Selection{},
		Instances:  map[*ast.Ident]types.Instance{},
		Implicits:  map[ast.Node]types.Object{},
	}
	conf := types.Config{FakeImportC: true, Importer: importer.ForCompiler(p.fset, "source", nil), Error: func(error) {}}
	p.pkg, _ = conf.Check("main", p.fset, p.files, p.info)
	for _, f := range p.files {
		for _, d := range f.Decls {
			fd, ok := d.(*ast.FuncDecl)
			if !ok {
				continue
			}
			p.funcs[funcKey(fd)] = fd
		}
	}
	return p
}

func funcKey(fd *ast.FuncDecl) string {
	if fd.Recv == nil || len(fd.Recv.List) == 0 {
		return fd.Name.Name
	}
	t := fd.Recv.List[0].Type
	if s, ok := t.(*ast.StarExpr); ok {
		t = s.X
	}
	if ix, ok := t.(*ast.IndexExpr); ok {
		t = ix.X
	}
	return t.(*ast.Ident).Name + "." + fd.Name.Name
}
