package main

import (
	"fmt"
	"go/ast"
	"go/token"
	"go/types"
	"os"
	"path/filepath"
	"strings"
)

// go:embed: package variables of type string or []byte initialised with a
// file's contents. The bytes become a static array in the C file and the
// variable is set first thing in ik_init_globals.
type embedVar struct {
	v    *types.Var
	file string
}

func embedDirective(groups ...*ast.CommentGroup) string {
	for _, g := range groups {
		if g == nil {
			continue
		}
		for _, c := range g.List {
			if rest, ok := strings.CutPrefix(c.Text, "//go:embed "); ok {
				return strings.TrimSpace(rest)
			}
		}
	}
	return ""
}

func findEmbeds(p *Prog, files []*ast.File) []embedVar {
	var out []embedVar
	for _, f := range files {
		dir := filepath.Dir(p.fset.Position(f.Pos()).Filename)
		for _, d := range f.Decls {
			gd, ok := d.(*ast.GenDecl)
			if !ok || gd.Tok != token.VAR {
				continue
			}
			for _, sp := range gd.Specs {
				vs := sp.(*ast.ValueSpec)
				var pat string
				if len(gd.Specs) == 1 {
					pat = embedDirective(gd.Doc, vs.Doc)
				} else {
					pat = embedDirective(vs.Doc)
				}
				if pat == "" || len(vs.Names) != 1 || strings.ContainsAny(pat, "*? ") {
					continue
				}
				v, _ := p.info.Defs[vs.Names[0]].(*types.Var)
				if v == nil {
					continue
				}
				out = append(out, embedVar{v, filepath.Join(dir, pat)})
			}
		}
	}
	return out
}

// emitEmbeds: the static data arrays, and the statements for ik_init_globals.
func emitEmbeds(embeds []embedVar) (decls string, inits []string) {
	var b strings.Builder
	for i, e := range embeds {
		if !usedGlobals[e.v] {
			continue
		}
		data, err := os.ReadFile(e.file)
		if err != nil {
			fmt.Fprintf(os.Stderr, "go:embed %s: %v\n", e.file, err)
			continue
		}
		name := fmt.Sprintf("ik_embed_%d", i)
		fmt.Fprintf(&b, "// go:embed %s\nstatic const unsigned char %s[%d] = {", filepath.Base(e.file), name, len(data)+1)
		for j, c := range append(data, 0) {
			if j%24 == 0 {
				b.WriteString("\n")
			}
			fmt.Fprintf(&b, "%d,", c)
		}
		b.WriteString("\n};\n")
		n := len(data)
		if basicKind(e.v.Type()) == types.String {
			inits = append(inits, fmt.Sprintf("%s = (GoString){(const char *)%s, %d};", gName(e.v), name, n))
		} else {
			inits = append(inits, fmt.Sprintf("{ u8 *_e = (u8 *)gc_alloc_atomic(%d); memcpy(_e, %s, %d); %s = (%s){.p = _e, .len = %d, .cap = %d}; }",
				n+1, name, n, gName(e.v), cbase(e.v.Type()), n, n))
		}
	}
	return b.String(), inits
}
