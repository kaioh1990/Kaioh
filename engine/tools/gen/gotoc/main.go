package main

import (
	"fmt"
	"go/ast"
	"go/types"
	"os"
	"runtime/debug"
	"sort"
	"strings"
)

// gotoc SRCDIR FUNC... : translate the given functions (Name or Type.Method)
// to C on stdout. "all:FILE.go" translates every function in that file.
func main() {
	p := load(os.Args[1])
	pkg = p.pkg
	ownPkgs[p.pkg] = true
	for pk := range p.own {
		ownPkgs[pk] = true
	}
	if b, err := os.ReadFile(os.Getenv("GENCDEFS_OVERRIDES")); err == nil {
		for _, ln := range strings.Split(string(b), "\n") {
			f := strings.Fields(ln)
			if len(f) >= 2 && !strings.HasPrefix(ln, "#") {
				overrides[f[0]] = strings.Join(f[1:], " ")
			}
		}
	}
	for _, f := range strings.Split(os.Getenv("GOTOC_INCLUDE_FUNCS"), ",") {
		if f != "" {
			includedFuncs[f] = true
		}
	}
	for _, f := range strings.Split(os.Getenv("GOTOC_NATIVE_FUNCS"), ",") {
		if f != "" {
			excludedFuncs[f] = true
			nativeFuncs[f] = true
		}
	}
	for _, f := range strings.Split(os.Getenv("GOTOC_EXCLUDE_FUNCS"), ",") {
		if f != "" {
			excludedFuncs[f] = true
		}
	}
	for _, f := range strings.Split(os.Getenv("GOTOC_EXCLUDE"), ",") {
		if f != "" {
			excludedFiles[f] = true
		}
	}
	var keys []string
	var initKeys []string // init functions of translated packages always run
	for _, pk := range p.pkgOrder {
		initKeys = append(initKeys, p.initFuncs[pk]...)
	}
	for _, a := range os.Args[2:] {
		if strings.HasPrefix(a, "reach:") {
			for k := range reach(p, append(strings.Split(strings.TrimPrefix(a, "reach:"), ","), initKeys...)) {
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
	sort.Slice(keys, func(i, j int) bool { // reproducible output: by file, then position
		pi, pj := p.fset.Position(p.funcs[keys[i]].Pos()), p.fset.Position(p.funcs[keys[j]].Pos())
		if fi, fj := fileKey(pi.Filename), fileKey(pj.Filename); fi != fj {
			return fi < fj
		}
		return pi.Offset < pj.Offset
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
		if fd.Type.TypeParams != nil || isGenericMethodDecl(fd) {
			continue // generic: hand-written helpers
		}
		tx := &Tx{p: p}
		var body string
		if isExcluded(p, k) {
			hdr, sig := tx.funcHeader(fd)
			protos = append(protos, hdr+";")
			if nativeFuncs[k] {
				continue // hand-written in src/ik (no stub)
			}
			ret := ""
			if sig.Results().Len() == 1 && isInvalid(sig.Results().At(0).Type()) {
				stubs = append(stubs, fmt.Sprintf("%s { return (GoOpaque){{0}}; } /* stub: %s */", hdr, k))
				continue
			}
			if sig.Results().Len() > 1 {
				// tuple: pointer results get fresh zero objects (constructors)
				var parts []string
				for i := 0; i < sig.Results().Len(); i++ {
					rt := sig.Results().At(i).Type()
					if pt, ok := rt.Underlying().(*types.Pointer); ok && cbase(pt.Elem()) != "void" && !isInvalid(rt) {
						parts = append(parts, fmt.Sprintf(".r%d = go_new(%s)", i, cbase(pt.Elem())))
					}
				}
				if len(parts) == 0 {
					parts = []string{"0"}
				}
				ret = " return (" + tx.retTypeOf(sig.Results(), "") + "){" + strings.Join(parts, ", ") + "};"
			} else if sig.Results().Len() > 0 {
				ret = " return (" + tx.retTypeOf(sig.Results(), cFuncName(p, p.info.Defs[fd.Name].(*types.Func))) + "){0};"
				if sig.Results().Len() == 1 {
					rt := sig.Results().At(0).Type()
					switch u := rt.Underlying().(type) {
					case *types.Pointer:
						// stubs return a fresh zero object, not NULL (constructors)
						ret = " return go_new(" + cbase(u.Elem()) + ");"
						if cbase(u.Elem()) == "void" {
							ret = " return NULL;"
						}
					case *types.Signature:
						ret = " return (GoFunc){0};"
					case *types.Basic:
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
				if os.Getenv("GOTOC_STACK") != "" {
					fmt.Fprintln(os.Stderr, string(debug.Stack()))
				}
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
	// ---- generic instances (may register more instances)
	declOf := map[*types.Func]*ast.FuncDecl{}
	for _, fd := range p.funcs {
		if fn, ok := p.info.Defs[fd.Name].(*types.Func); ok {
			declOf[fn] = fd
		}
	}
	translateInstances := func() {
		for len(genPending) > 0 {
			gi := genPending[0]
			genPending = genPending[1:]
			fd := declOf[gi.fn]
			if fd == nil || fd.Body == nil {
				continue
			}
			curSubst = bindTypeParams(gi.fn, gi.targs)
			tx := &Tx{p: p, instName: gi.name}
			var body string
			func() {
				defer func() {
					if r := recover(); r != nil {
						fmt.Fprintf(os.Stderr, "translator crash in %s: %v\n", gi.name, r)
						body = "/* translator crash */"
					}
				}()
				body = tx.translateFunc(fd)
			}()
			hdr, _ := tx.funcHeader(fd)
			curSubst = nil
			protos = append(protos, hdr+";")
			total += tx.todo
			bodies = append(bodies, fmt.Sprintf("// %s (generic instance), %d TODO\n%s", gi.name, tx.todo, body))
		}
	}
	translateInstances()

	// ---- package variables and their initializers (Go init order)
	initTx := &Tx{p: p, labels: map[string]bool{}}
	initTx.out = new(bytesBuffer)
	initTx.w("void ik_init_globals(void) {")
	initTx.ind++
	embedDecls, embedInits := emitEmbeds(findEmbeds(p, p.files))
	for _, s := range embedInits {
		initTx.w("%s", s)
	}
	// Go order: per package (dependencies first), variables then init()
	for _, pk := range p.pkgOrder {
		for _, ini := range p.initOrder {
			if ini.Lhs[0].Pkg() != pk {
				continue
			}
			used := false
			for _, l := range ini.Lhs {
				if usedGlobals[l] {
					used = true
				}
			}
			if !used {
				continue
			}
			if len(ini.Lhs) == 1 {
				initTx.w("%s = %s;", gName(ini.Lhs[0]), initTx.exprConv(ini.Rhs, ini.Lhs[0].Type()))
			} else {
				initTx.w("%s;", initTx.TODO(ini.Rhs, "multi-value package initializer"))
			}
		}
		for _, k := range p.initFuncs[pk] {
			initTx.w("%s();", initFuncName(p.info.Defs[p.funcs[k].Name].(*types.Func)))
		}
	}
	initTx.ind--
	initTx.w("}")
	var gvars []*types.Var
	for v := range usedGlobals {
		gvars = append(gvars, v)
	}
	sort.Slice(gvars, func(i, j int) bool { return gvars[i].Name() < gvars[j].Name() })
	rtTables := emitRTypes(p)
	registerRuntimeTypes(p)
	dprotos, dbodies := emitDispatchers(p)
	translateInstances() // methods of generic types reached through interfaces
	binAny := binreadAny()
	ext := externProtos(p)
	var rn []string
	for n := range multiRets {
		rn = append(rn, n)
	}
	sort.Strings(rn)
	for _, v := range gvars {
		cbase(v.Type())
	}
	pre := typePreamble()

	// ---- header
	var h strings.Builder
	h.WriteString("// Generated by tools/gen/gotoc from Ikemen GO (reachable simulation). Do not edit.\n")
	h.WriteString("#ifndef IK_SIM_H\n#define IK_SIM_H\n#include \"platform/bind.h\"\n#include \"core/gen_types.h\"\n#include \"core/gen_consts.h\"\n#include \"ik/ik_rt_types.h\"\n\n")
	h.WriteString(pre + "\n")
	for _, n := range rn {
		fmt.Fprintf(&h, "#ifndef CVF_RET_%s\n#define CVF_RET_%s\n%s\n#endif\n", n, n, multiRets[n])
	}
	h.WriteString("\n#include \"ik/ik_rt.h\"\n\n// std-library entry points (ik_rt.c)\n")
	for _, e := range ext {
		h.WriteString(e + "\n")
	}
	h.WriteString("\n// package variables\n")
	for _, v := range gvars {
		fmt.Fprintf(&h, "extern %s %s;\n", cbase(v.Type()), gName(v))
	}
	h.WriteString("void ik_init_globals(void);\n\n")
	h.WriteString("GoIface ik_binread_any(GoIface r, GoIface x, int big);\n")
	h.WriteString(tidDefines(p))
	h.WriteString(implFuncs())
	for _, pr := range protos {
		h.WriteString(pr + "\n")
	}
	for _, pr := range dprotos {
		h.WriteString(pr + "\n")
	}
	h.WriteString("#endif\n")
	os.WriteFile(os.Getenv("GOTOC_HDR"), []byte(h.String()), 0o644)

	// ---- source
	fmt.Println("// Generated by tools/gen/gotoc from Ikemen GO (reachable simulation). Do not edit.")
	fmt.Println("#include \"ik/ik_sim.h\"")
	fmt.Println()
	for _, th := range eqFuncLines() {
		fmt.Println(th)
	}
	for _, th := range thunkLines() {
		fmt.Println(th)
	}
	for _, n := range binreadOrder {
		fmt.Println(binreadHelpers[n])
	}
	fmt.Println()
	for _, l := range lifted {
		fmt.Println(l)
	}
	fmt.Println()
	for _, b := range bodies {
		fmt.Println(b)
	}
	fmt.Println("// ---- package variables")
	for _, v := range gvars {
		fmt.Printf("%s %s;\n", cbase(v.Type()), gName(v))
	}
	fmt.Println(embedDecls)
	fmt.Println(initTx.out.String())
	fmt.Println(rtTables)
	fmt.Println(tidKindTable())
	fmt.Println(binAny)
	fmt.Println("// ---- interface dispatchers")
	for _, b := range dbodies {
		fmt.Println(b)
	}
	fmt.Println("// ---- stubs (excluded files)")
	for _, st := range stubs {
		fmt.Println(st)
	}
	fmt.Fprintf(os.Stderr, "%d functions, %d TODO, %d stubs\n", len(bodies), total, len(stubs))
}
