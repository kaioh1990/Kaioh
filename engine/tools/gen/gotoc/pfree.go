package main

import "go/types"

// pointerFree reports whether values of t can never hold a pointer into the
// collected heap, so their storage may be allocated "atomic" (never scanned).
// It errs towards false: strings, slices, maps, funcs, interfaces, pointers,
// uintptr, unsafe.Pointer, opaque/foreign types and type parameters without a
// substitution all count as pointer-holding.
func pointerFree(t types.Type) bool { return pfree(t, map[types.Type]bool{}) }

// types whose C layout is hand-written/opaque (gencdefs overrides.txt)
var pfreeOpaque = map[string]bool{"Snd": true, "Fnt": true, "Texture": true, "lazySound": true, "Sound": true, "FntCharImage": true}

func pfree(t types.Type, seen map[types.Type]bool) bool {
	t = types.Unalias(t)
	if tp, ok := t.(*types.TypeParam); ok {
		if s, ok := pfreeSubst(tp); ok {
			return pfree(s, seen)
		}
		return false
	}
	if seen[t] {
		return true
	}
	seen[t] = true
	if n, ok := t.(*types.Named); ok {
		if pfreeOpaque[n.Obj().Name()] {
			return false
		}
		if _, basic := n.Underlying().(*types.Basic); !basic && (n.Obj().Pkg() == nil || n.Obj().Pkg().Name() != "main") {
			return false // foreign struct: opaque or hand-written layout
		}
	}
	switch u := t.Underlying().(type) {
	case *types.Basic:
		switch u.Kind() {
		case types.String, types.UntypedString, types.UnsafePointer, types.Uintptr, types.UntypedNil, types.Invalid:
			return false
		}
		return true
	case *types.Array:
		return pfree(u.Elem(), seen)
	case *types.Struct:
		for i := 0; i < u.NumFields(); i++ {
			if !pfree(u.Field(i).Type(), seen) {
				return false
			}
		}
		return true
	}
	return false
}

func pfreeSubst(tp *types.TypeParam) (types.Type, bool) {
	s, ok := curSubst[tp]
	return s, ok
}

func boxFn(t types.Type) string {
	if pointerFree(t) {
		return "go_box_a"
	}
	return "go_box"
}

func sliceFuncsMacro(elem types.Type) string {
	if pointerFree(elem) {
		return "GO_SLICE_FUNCS_A"
	}
	return "GO_SLICE_FUNCS"
}
