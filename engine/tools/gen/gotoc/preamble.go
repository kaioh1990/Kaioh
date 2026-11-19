package main

import (
	"fmt"
	"go/types"
	"os"
	"sort"
	"strings"
)

// typePreamble declares every slice/map/array type the translation uses,
// guarded with the same macros as gen_types.h so the two never collide.
func typePreamble() string {
	var b strings.Builder
	keys := func(m map[string]bool) []string {
		var o []string
		for k := range m {
			o = append(o, k)
		}
		sort.Strings(o)
		return o
	}
	// cbase can discover new types while we emit; iterate to a fixed point.
	seen := map[string]bool{}
	for {
		n0 := len(sliceTypes) + len(mapTypes) + len(arrTypes) + len(anonStructs) + len(localTypes)
		fields := func(st *types.Struct) {
			for i := 0; i < st.NumFields(); i++ {
				cbase(st.Field(i).Type())
			}
		}
		for _, st := range anonStructs {
			fields(st)
		}
		for _, lt := range localTypes {
			if st, ok := lt.Underlying().(*types.Struct); ok {
				fields(st)
			} else {
				cbase(lt.Underlying())
			}
		}
		for n, el := range sliceTypes {
			cbase(el)
			seen[n] = true
		}
		for _, m := range mapTypes {
			cbase(m.Key())
			cbase(m.Elem())
		}
		for _, a := range arrTypes {
			cbase(a.Elem())
		}
		if len(sliceTypes)+len(mapTypes)+len(arrTypes)+len(anonStructs)+len(localTypes) == n0 {
			break
		}
	}
	sn := map[string]bool{}
	for n := range sliceTypes {
		sn[n] = true
	}
	mn := map[string]bool{}
	for n := range mapTypes {
		mn[n] = true
	}
	an := map[string]bool{}
	for n := range arrTypes {
		an[n] = true
	}
	for _, n := range keys(an) {
		fmt.Fprintf(&b, "#ifndef CVF_DEF_%s\n#define CVF_DEF_%s\ntypedef struct %s %s;\n#endif\n", n, n, n, n)
	}
	genDefined := genTypesDefined()
	// local named types and anonymous structs
	ln := map[string]bool{}
	for n := range localTypes {
		ln[n] = true
	}
	for _, n := range keys(ln) {
		if _, ok := localTypes[n].Underlying().(*types.Struct); ok {
			fmt.Fprintf(&b, "typedef struct %s %s;\n", n, n)
		}
	}
	anm := map[string]bool{}
	for _, n := range anonOrder {
		if !genDefined[n] {
			anm[n] = true
		}
	}
	for _, n := range keys(anm) {
		fmt.Fprintf(&b, "typedef struct %s %s;\n", n, n)
	}
	// named non-struct types whose C type needs no slice/map/array typedef
	// (interfaces, numbers, funcs, pointers) come before the slices of them
	earlyLocal := map[string]bool{}
	for _, n := range keys(ln) {
		switch localTypes[n].Underlying().(type) {
		case *types.Interface, *types.Basic, *types.Signature, *types.Pointer:
			fmt.Fprintf(&b, "typedef %s %s;\n", cbase(localTypes[n].Underlying()), n)
			earlyLocal[n] = true
		}
	}
	for _, n := range keys(sn) {
		fmt.Fprintf(&b, "#ifndef CVF_DEF_%s\n#define CVF_DEF_%s\ntypedef struct { %s *p; int len, cap; } %s;\n#endif\n", n, n, cbase(sliceTypes[n]), n)
	}
	for _, n := range keys(mn) {
		fmt.Fprintf(&b, "#ifndef CVF_DEF_%s\n#define CVF_DEF_%s\ntypedef GoMap %s;\n#endif\n", n, n, n)
	}
	for _, n := range keys(ln) {
		if _, ok := localTypes[n].Underlying().(*types.Struct); !ok && !earlyLocal[n] {
			fmt.Fprintf(&b, "typedef %s %s;\n", cbase(localTypes[n].Underlying()), n)
		}
	}
	// array bodies, innermost first
	done := map[string]bool{}
	var emitArr func(n string)
	emitArr = func(n string) {
		if done[n] {
			return
		}
		done[n] = true
		a := arrTypes[n]
		fmt.Fprintf(&b, "#ifndef CVF_BODY_%s\n#define CVF_BODY_%s\nstruct %s { %s v[%d]; };\n#endif\n", n, n, n, cbase(a.Elem()), a.Len())
	}
	structDone := map[string]bool{}
	var emitVal func(ty types.Type)
	emitStruct := func(n string, st *types.Struct) {
		if structDone[n] || genDefined[n] {
			return
		}
		structDone[n] = true
		for i := 0; i < st.NumFields(); i++ {
			emitVal(st.Field(i).Type())
		}
		prom := promotedNames(st)
		fmt.Fprintf(&b, "struct %s {", n)
		if st.NumFields() == 0 {
			b.WriteString(" char _empty;")
		}
		for i := 0; i < st.NumFields(); i++ {
			f := st.Field(i)
			if es, ok := f.Type().Underlying().(*types.Struct); ok && f.Embedded() {
				fmt.Fprintf(&b, " union { %s %s; struct {", cbase(f.Type()), embName(f))
				for k := 0; k < es.NumFields(); k++ {
					fn := embName(es.Field(k))
					if !prom[es.Field(k).Name()] {
						fn = fmt.Sprintf("_sh%d_%s", i, es.Field(k).Name())
					}
					fmt.Fprintf(&b, " %s %s;", cbase(es.Field(k).Type()), fn)
				}
				if es.NumFields() == 0 {
					b.WriteString(" char _empty_" + f.Name() + ";")
				}
				b.WriteString(" }; };")
				continue
			}
			fmt.Fprintf(&b, " %s %s;", cbase(f.Type()), embName(f))
		}
		b.WriteString(" };\n")
	}
	emitVal = func(ty types.Type) {
		switch u := ty.(type) {
		case *types.Named:
			n := cbase(u)
			if _, ok := localTypes[n]; ok {
				switch uu := u.Underlying().(type) {
				case *types.Struct:
					emitStruct(n, uu)
				case *types.Array:
					emitVal(uu)
				}
			}
		case *types.Array:
			n := cbase(u)
			if _, ok := arrTypes[n]; ok {
				emitVal(u.Elem())
				emitArr(n)
			}
		case *types.Struct:
			emitStruct(anonName(u), u)
		}
	}
	for _, n := range keys(an) {
		emitVal(arrTypes[n])
	}
	for _, n := range keys(ln) {
		emitVal(localTypes[n])
	}
	for _, n := range keys(anm) {
		emitVal(anonStructs[n])
	}
	for _, n := range keys(sn) {
		el := cbase(sliceTypes[n])
		if el == "void" {
			continue
		}
		fmt.Fprintf(&b, "#ifndef CVF_FN_%s\n#define CVF_FN_%s\n%s(%s, %s)\n#endif\n", n, n, sliceFuncsMacro(sliceTypes[n]), n, el)
	}
	for _, n := range keys(mn) {
		mt := mapTypes[n]
		k, v := cbase(mt.Key()), cbase(mt.Elem())
		if k == "void" || v == "void" {
			continue
		}
		sk := "false"
		if isString(mt.Key()) {
			sk = "true"
		}
		fmt.Fprintf(&b, "#ifndef CVF_FN_%s\n#define CVF_FN_%s\nGO_MAP_FUNCS(%s, %s, %s, %s)\n#endif\n", n, n, n, k, v, sk)
	}
	return b.String()
}

// struct bodies and typedefs gen_types.h already provides
func genTypesDefined() map[string]bool {
	out := map[string]bool{}
	path := os.Getenv("GOTOC_GEN_TYPES")
	if path == "" {
		return out
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return out
	}
	for _, ln := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(ln, "struct ") && strings.HasSuffix(ln, " {") {
			out[strings.TrimSuffix(strings.TrimPrefix(ln, "struct "), " {")] = true
		}
	}
	return out
}

// promotedNames: names of embedded structs' fields that Go promotes into
// st (not shadowed by a direct field, not present in two embeddings).
// Others keep their slot in the flattened layout under a private name.
func promotedNames(st *types.Struct) map[string]bool {
	direct := map[string]bool{}
	count := map[string]int{}
	for i := 0; i < st.NumFields(); i++ {
		f := st.Field(i)
		direct[f.Name()] = true
		if es, ok := f.Type().Underlying().(*types.Struct); ok && f.Embedded() {
			for k := 0; k < es.NumFields(); k++ {
				count[es.Field(k).Name()]++
			}
		}
	}
	out := map[string]bool{}
	for n, c := range count {
		if c == 1 && !direct[n] {
			out[n] = true
		}
	}
	return out
}
