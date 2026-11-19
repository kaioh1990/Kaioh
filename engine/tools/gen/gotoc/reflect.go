package main

import (
	"fmt"
	"go/types"
	"sort"
	"strings"
)

// Type descriptors for the reflect runtime (src/ik/ik_reflect.c): one per
// type that can reach reflect through an interface value, plus everything
// they reference (element, key and field types).

var rtIndex = map[string]int{}
var rtList []types.Type

func rtKey(ty types.Type) string { return canonTypeKey(ty) }

func rtypeOf(ty types.Type) int {
	ty = types.Unalias(ty)
	k := rtKey(ty)
	if i, ok := rtIndex[k]; ok {
		return i
	}
	i := len(rtList)
	rtIndex[k] = i
	rtList = append(rtList, ty)
	switch u := ty.Underlying().(type) {
	case *types.Pointer:
		rtypeOf(u.Elem())
	case *types.Slice:
		rtypeOf(u.Elem())
	case *types.Array:
		rtypeOf(u.Elem())
	case *types.Map:
		rtypeOf(u.Key())
		rtypeOf(u.Elem())
	case *types.Struct:
		for f := 0; f < u.NumFields(); f++ {
			rtypeOf(u.Field(f).Type())
		}
	}
	return i
}

func reflectKind(ty types.Type) int {
	switch u := ty.Underlying().(type) {
	case *types.Basic:
		switch u.Kind() {
		case types.Bool:
			return 1
		case types.Int:
			return 2
		case types.Int8:
			return 3
		case types.Int16:
			return 4
		case types.Int32:
			return 5
		case types.Int64:
			return 6
		case types.Uint:
			return 7
		case types.Uint8:
			return 8
		case types.Uint16:
			return 9
		case types.Uint32:
			return 10
		case types.Uint64:
			return 11
		case types.Uintptr:
			return 12
		case types.Float32:
			return 13
		case types.Float64:
			return 14
		case types.String:
			return 24
		case types.UnsafePointer:
			return 26
		}
	case *types.Array:
		return 17
	case *types.Chan:
		return 18
	case *types.Signature:
		return 19
	case *types.Interface:
		return 20
	case *types.Map:
		return 21
	case *types.Pointer:
		return 22
	case *types.Slice:
		return 23
	case *types.Struct:
		return 25
	}
	return 0
}

func cStr(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c < 0x20 || c >= 0x7f:
			fmt.Fprintf(&b, "\\%03o", c)
		default:
			b.WriteByte(c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

// emitRTypes describes every boxed type (to a fixed point: describing a
// type assigns interface ids to the types it references).
func emitRTypes(p *Prog) string {
	for {
		n0 := len(typeByID)
		var ids []int
		for id := range typeByID {
			ids = append(ids, id)
		}
		sort.Ints(ids)
		for _, id := range ids {
			rtypeOf(typeByID[id])
		}
		for _, ty := range rtList {
			if !isInvalid(ty) && !isIface(ty) {
				typeID(ty)
			}
		}
		if len(typeByID) == n0 {
			break
		}
	}
	var b strings.Builder
	n := len(rtList)
	fmt.Fprintf(&b, "const RType ik_rt[%d];\n", n)
	// fields
	for i, ty := range rtList {
		st, ok := ty.Underlying().(*types.Struct)
		if !ok || st.NumFields() == 0 || isInvalid(ty) {
			continue
		}
		c := cbase(ty)
		if strings.Contains(c, "/*") || strings.HasPrefix(c, "GoOpaque") || strings.HasPrefix(c, "Go") {
			continue
		}
		fmt.Fprintf(&b, "static const RField ik_rf_%d[] = {", i)
		for f := 0; f < st.NumFields(); f++ {
			fv := st.Field(f)
			off := fmt.Sprintf("offsetof(%s, %s)", c, embName(fv))
			if fv.Name() == "_" {
				off = "0"
			}
			pkg := ""
			if !fv.Exported() && fv.Pkg() != nil {
				pkg = fv.Pkg().Path()
			}
			fmt.Fprintf(&b, "{%s, %s, %s, &ik_rt[%d], %s, %v}, ", cStr(fv.Name()), cStr(pkg), cStr(st.Tag(f)),
				rtIndex[rtKey(fv.Type())], off, fv.Embedded())
		}
		b.WriteString("};\n")
	}
	fmt.Fprintf(&b, "const RType ik_rt[%d] = {\n", n)
	for i, ty := range rtList {
		kind := reflectKind(ty)
		c := cbase(ty)
		size := "sizeof(" + c + ")"
		if c == "void" || isInvalid(ty) {
			size = "0"
		}
		name, pkg := "", ""
		if nm, ok := ty.(*types.Named); ok {
			name = nm.Obj().Name()
			if nm.Obj().Pkg() != nil {
				pkg = nm.Obj().Pkg().Path()
			}
		} else if bt, ok := ty.(*types.Basic); ok {
			name = bt.Name()
		}
		elem, key, alen, nf, fields, strkey := "NULL", "NULL", "0", 0, "NULL", "false"
		switch u := ty.Underlying().(type) {
		case *types.Pointer:
			elem = fmt.Sprintf("&ik_rt[%d]", rtIndex[rtKey(u.Elem())])
		case *types.Slice:
			elem = fmt.Sprintf("&ik_rt[%d]", rtIndex[rtKey(u.Elem())])
		case *types.Array:
			elem = fmt.Sprintf("&ik_rt[%d]", rtIndex[rtKey(u.Elem())])
			alen = fmt.Sprint(u.Len())
		case *types.Map:
			key = fmt.Sprintf("&ik_rt[%d]", rtIndex[rtKey(u.Key())])
			elem = fmt.Sprintf("&ik_rt[%d]", rtIndex[rtKey(u.Elem())])
			if isString(u.Key()) {
				strkey = "true"
			}
		case *types.Struct:
			if u.NumFields() > 0 && !strings.Contains(c, "/*") && !strings.HasPrefix(c, "Go") {
				nf = u.NumFields()
				fields = fmt.Sprintf("ik_rf_%d", i)
			}
		}
		tid := 0
		if !isInvalid(ty) && !isIface(ty) {
			tid = typeID(ty)
		}
		fmt.Fprintf(&b, "    {%d, %s, %s, %s, %s, %s, %s, %d, %s, %d, %s, %s},\n", kind, size, cStr(name), cStr(pkg),
			elem, key, alen, nf, fields, tid, strkey, cStr(types.TypeString(ty, func(p *types.Package) string { return p.Name() })))
	}
	b.WriteString("};\nconst int ik_rt_count = " + fmt.Sprint(n) + ";\n")
	// type id -> descriptor
	maxID := 0
	for id := range typeByID {
		if id > maxID {
			maxID = id
		}
	}
	slots := make([]string, maxID+1)
	for i := range slots {
		slots[i] = "NULL"
	}
	for id, ty := range typeByID {
		if j, ok := rtIndex[rtKey(ty)]; ok {
			slots[id] = fmt.Sprintf("&ik_rt[%d]", j)
		}
	}
	fmt.Fprintf(&b, "const RType *const ik_rt_by_tid[%d] = {%s};\nconst int ik_rt_by_tid_count = %d;\n", maxID+1, strings.Join(slots, ", "), maxID+1)
	return b.String()
}

// reflect.Type is an interface whose only dynamic type is the runtime's
// descriptor (GO_TID_RTYPE): dispatchers route its methods to rt_type_*.
func isReflectTypeMethod(fn *types.Func) bool {
	sig := fn.Type().(*types.Signature)
	r := sig.Recv()
	if r == nil {
		return false
	}
	n, ok := types.Unalias(r.Type()).(*types.Named)
	return ok && n.Obj().Pkg() != nil && n.Obj().Pkg().Path() == "reflect" && n.Obj().Name() == "Type"
}

// reflect.Type methods called through interfaces (rt_type_* must exist)
var rtTypeMethodsUsed = map[string]string{}
