package main

import (
	"fmt"
	"go/types"
	"os"
	"sort"
	"strings"
)

// gotoc SRCDIR FUNC... : translate the given functions (Name or Type.Method)
// to C on stdout. "all:FILE.go" translates every function in that file.
func main() {
	p := load(os.Args[1])
	pkg = p.pkg
	if b, err := os.ReadFile(os.Getenv("GENCDEFS_OVERRIDES")); err == nil {
		for _, ln := range strings.Split(string(b), "\n") {
			f := strings.Fields(ln)
			if len(f) >= 2 && !strings.HasPrefix(ln, "#") {
				overrides[f[0]] = strings.Join(f[1:], " ")
			}
		}
	}
	for _, f := range strings.Split(os.Getenv("GOTOC_EXCLUDE"), ",") {
		if f != "" {
			excludedFiles[f] = true
		}
	}
	var keys []string
	for _, a := range os.Args[2:] {
		if strings.HasPrefix(a, "reach:") {
			for k := range reach(p, strings.Split(strings.TrimPrefix(a, "reach:"), ",")) {
				keys = append(keys, k)
			}
			continue
		}
		if strings.HasPrefix(a, "all:") {
			file := strings.TrimPrefix(a, "all:")
			for _, f := range p.files {
				if shortFile(p.fset.Position(f.Pos()).Filename) != file {
					continue
				}
				for k, fd := range p.funcs {
					if p.fset.Position(fd.Pos()).Filename == p.fset.Position(f.Pos()).Filename {
						keys = append(keys, k)
					}
				}
			}
			continue
		}
		keys = append(keys, a)
	}
	sort.Slice(keys, func(i, j int) bool {
		return p.fset.Position(p.funcs[keys[i]].Pos()).Offset < p.fset.Position(p.funcs[keys[j]].Pos()).Offset ||
			p.fset.Position(p.funcs[keys[i]].Pos()).Filename < p.fset.Position(p.funcs[keys[j]].Pos()).Filename
	})
	total := 0
	var stubs []string
	var bodies []string
	var protos []string
	for _, k := range keys {
		fd := p.funcs[k]
		if fd == nil {
			fmt.Fprintln(os.Stderr, "no function", k)
			continue
		}
		if fd.Type.TypeParams != nil {
			continue // generic: hand-written helpers
		}
		tx := &Tx{p: p}
		var body string
		if isExcluded(p, k) {
			hdr, sig := tx.funcHeader(fd)
			protos = append(protos, hdr+";")
			ret := ""
			if sig.Results().Len() > 0 {
				ret = " return (" + tx.retTypeOf(sig.Results(), cFuncName(p, p.info.Defs[fd.Name].(*types.Func))) + "){0};"
				if sig.Results().Len() == 1 {
					rt := sig.Results().At(0).Type()
					switch rt.Underlying().(type) {
					case *types.Basic, *types.Pointer, *types.Signature:
						ret = " return 0;"
						if isString(rt) {
							ret = " return (GoString){0};"
						}
					}
				}
			}
			stubs = append(stubs, fmt.Sprintf("%s {%s } /* stub: %s */", hdr, ret, k))
			continue
		}
		func() {
			defer func() {
				if r := recover(); r != nil {
					fmt.Fprintf(os.Stderr, "translator crash in %s: %v\n", k, r)
					body = "/* translator crash: " + fmt.Sprint(r) + " */"
					tx.todo++
				}
			}()
			body = tx.translateFunc(fd)
		}()
		hdr, _ := tx.funcHeader(fd)
		protos = append(protos, hdr+";")
		total += tx.todo
		pos := p.fset.Position(fd.Pos())
		bodies = append(bodies, fmt.Sprintf("// %s (%s:%d), %d TODO\n%s", k, shortFile(pos.Filename), pos.Line, tx.todo, body))
	}
	var rn []string
	for n := range multiRets {
		rn = append(rn, n)
	}
	sort.Strings(rn)
	for _, n := range rn {
		fmt.Println(multiRets[n])
	}
	fmt.Println()
	for _, pr := range protos {
		fmt.Println(pr)
	}
	fmt.Println()
	for _, l := range lifted {
		fmt.Println(l)
	}
	fmt.Println()
	for _, b := range bodies {
		fmt.Println(b)
	}
	fmt.Println("// ---- stubs (excluded files)")
	for _, st := range stubs {
		fmt.Println(st)
	}
	fmt.Fprintf(os.Stderr, "%d functions, %d TODO, %d stubs\n", len(bodies), total, len(stubs))
}
