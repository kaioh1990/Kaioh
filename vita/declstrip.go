// declstrip FILE IDENT... : removes top-level decls whose source mentions any IDENT.
package main

import (
	"bytes"
	"go/ast"
	"go/format"
	"go/parser"
	"go/token"
	"os"
	"strings"
)

func main() {
	fn := os.Args[1]
	idents := os.Args[2:]
	src, _ := os.ReadFile(fn)
	fs := token.NewFileSet()
	f, err := parser.ParseFile(fs, fn, src, parser.ParseComments)
	if err != nil {
		panic(err)
	}
	var keep []ast.Decl
	for _, d := range f.Decls {
		txt := string(src[fs.Position(d.Pos()).Offset:fs.Position(d.End()).Offset])
		drop := false
		for _, id := range idents {
			if strings.Contains(txt, id) {
				drop = true
			}
		}
		if g, ok := d.(*ast.GenDecl); ok && g.Tok == token.IMPORT {
			drop = false
		}
		if !drop {
			keep = append(keep, d)
		}
	}
	f.Decls = keep
	// drop comments that belonged to removed decls (keep only cgo preamble etc.)
	var cg []*ast.CommentGroup
	for _, c := range f.Comments {
		in := c.Pos() < f.Package
		for _, d := range keep {
			if c.Pos() >= d.Pos()-1 && c.End() <= d.End() {
				in = true
			}
			if gd, ok := d.(*ast.GenDecl); ok && gd.Doc == c {
				in = true
			}
			if fd, ok := d.(*ast.FuncDecl); ok && fd.Doc == c {
				in = true
			}
		}
		if in {
			cg = append(cg, c)
		}
	}
	f.Comments = cg
	var buf bytes.Buffer
	if err := format.Node(&buf, fs, f); err != nil {
		panic(err)
	}
	os.WriteFile(fn, buf.Bytes(), 0644)
}
