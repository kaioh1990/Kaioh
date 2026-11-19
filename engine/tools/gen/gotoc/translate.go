package main

import (
	"bytes"
	"fmt"
	"go/ast"

	"go/token"
	"go/types"
	"strings"
)

// Translator state for one function.
type bytesBuffer = bytes.Buffer

type Tx struct {
	p        *Prog
	captured map[types.Object]bool // inside a lifted closure: vars reached through _env
	outerFn  string
	out      *bytes.Buffer
	ind      int
	todo     int
	tmp      int
	fn       *ast.FuncDecl
	results  *types.Tuple
	retType  string
	labels   map[string]bool
	namedRes []string
	instName string // monomorphised generic instance being translated
	loopCont []string // continue labels of enclosing loops
	inSwitch int      // switch nesting inside the innermost loop
	heap     map[types.Object]bool // locals captured by closures (heap-allocated)
	stackBox map[types.Object]bool // ... of which only non-escaping closures capture (stack storage)
	nonEsc   map[*ast.FuncLit]bool // literals whose environment can live on the stack
	defers   []deferInfo
	deferDecl []string
}

func (t *Tx) w(format string, a ...any) {
	t.out.WriteString(strings.Repeat("    ", t.ind))
	fmt.Fprintf(t.out, format, a...)
	t.out.WriteByte('\n')
}

func (t *Tx) newTmp() string {
	t.tmp++
	return fmt.Sprintf("_t%d", t.tmp)
}

func (t *Tx) TODO(n ast.Node, what string) string {
	t.todo++
	pos := t.p.fset.Position(n.Pos())
	return fmt.Sprintf("TODO(/* %s at %s:%d */)", what, shortFile(pos.Filename), pos.Line)
}

func shortFile(f string) string {
	if i := strings.LastIndex(f, "/"); i >= 0 {
		return f[i+1:]
	}
	return f
}

func (t *Tx) typeOf(e ast.Expr) types.Type {
	if tv, ok := t.p.info.Types[e]; ok && tv.Type != nil {
		return substT(types.Unalias(tv.Type))
	}
	if id, ok := e.(*ast.Ident); ok {
		if o := t.p.info.Uses[id]; o != nil {
			return substT(o.Type())
		}
		if o := t.p.info.Defs[id]; o != nil {
			return substT(o.Type())
		}
	}
	return nil
}

// ---------------------------------------------------------------- names

func cFuncName(p *Prog, obj *types.Func) string {
	sig := obj.Type().(*types.Signature)
	if r := sig.Recv(); r != nil {
		rt := r.Type()
		if pt, ok := rt.(*types.Pointer); ok {
			rt = pt.Elem()
		}
		if n, ok := rt.(*types.Named); ok {
			if obj.Pkg() != nil && !isOwn(obj.Pkg()) {
				externs[obj] = true
				return obj.Pkg().Name() + "_" + n.Obj().Name() + "_" + obj.Name()
			}
			if obj.Pkg() != nil && obj.Pkg().Name() != "main" {
				return obj.Pkg().Name() + "_" + n.Obj().Name() + "_" + obj.Name()
			}
			return n.Obj().Name() + "_" + obj.Name()
		}
	}
	if !isOwn(obj.Pkg()) && obj.Pkg() != nil {
		externs[obj] = true
		return obj.Pkg().Name() + "_" + obj.Name()
	}
	if obj.Name() == "init" && sig.Recv() == nil {
		return initFuncName(obj)
	}
	if obj.Pkg() != nil && obj.Pkg().Name() != "main" {
		return obj.Pkg().Name() + "_" + obj.Name()
	}
	return "fn_" + obj.Name()
}

func cVarName(name string) string {
	switch name {
	case "char", "int", "float", "double", "long", "short", "signed", "unsigned", "auto", "register",
		"static", "extern", "volatile", "const", "struct", "union", "enum", "typedef", "sizeof",
		"default", "case", "do", "while", "for", "if", "else", "switch", "break", "continue",
		"return", "goto", "void", "inline", "restrict", "bool", "true", "false", "NULL", "main",
		"new", "delete", "abs", "min", "max", "exp", "log", "floor", "ceil", "sin", "cos", "tan", "pow",
		"round", "signbit", "isnan", "isinf", "errno", "stdin", "stdout", "stderr", "time", "random", "rand",
		"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "uintptr_t", "size_t",
		"GoString", "GoIface", "GoFunc", "GoMap", "GoOpaque", "memcpy", "memset", "strlen", "free", "malloc",
		"assert", "exit", "abort", "fabs", "sqrt", "trunc", "fmod", "remove", "rename", "puts", "printf":
		return name + "_"
	}
	return name
}

// ---------------------------------------------------------------- types

func (t *Tx) ctype(ty types.Type) string { return cbase(ty) }

func isPtr(ty types.Type) bool {
	_, ok := ty.Underlying().(*types.Pointer)
	return ok
}

func basicKind(ty types.Type) types.BasicKind {
	if b, ok := ty.Underlying().(*types.Basic); ok {
		return b.Kind()
	}
	return types.Invalid
}

func isString(ty types.Type) bool {
	if ty == nil {
		return false
	}
	b, ok := ty.Underlying().(*types.Basic)
	return ok && (b.Info()&types.IsString) != 0
}
func isFloat(ty types.Type) bool {
	b, ok := ty.Underlying().(*types.Basic)
	return ok && (b.Info()&types.IsFloat) != 0
}
func isInteger(ty types.Type) bool {
	b, ok := ty.Underlying().(*types.Basic)
	return ok && (b.Info()&types.IsInteger) != 0
}
func isUnsigned(ty types.Type) bool {
	b, ok := ty.Underlying().(*types.Basic)
	return ok && (b.Info()&types.IsUnsigned) != 0
}
func intBits(ty types.Type) int {
	switch basicKind(ty) {
	case types.Int8, types.Uint8:
		return 8
	case types.Int16, types.Uint16:
		return 16
	case types.Int32, types.Uint32, types.Int, types.Uint, types.UntypedRune:
		return 32
	case types.Int64, types.Uint64, types.Uintptr:
		return 64
	}
	return 32
}

// ---------------------------------------------------------------- functions

func (t *Tx) funcHeader(fd *ast.FuncDecl) (string, *types.Signature) {
	obj := t.p.info.Defs[fd.Name].(*types.Func)
	sig := obj.Type().(*types.Signature)
	var params []string
	if r := sig.Recv(); r != nil {
		name := "_recv"
		if r.Name() != "" && r.Name() != "_" {
			name = cVarName(r.Name())
		}
		params = append(params, t.ctype(r.Type())+" "+name)
	}
	for i := 0; i < sig.Params().Len(); i++ {
		v := sig.Params().At(i)
		name := cVarName(v.Name())
		if name == "" || name == "_" {
			name = fmt.Sprintf("_p%d", i)
		}
		ty := t.ctype(v.Type())
		if sig.Variadic() && i == sig.Params().Len()-1 {
			ty = t.ctype(v.Type()) // slice
		}
		params = append(params, ty+" "+name)
	}
	if len(params) == 0 {
		params = []string{"void"}
	}
	ret := t.retTypeOf(sig.Results(), cFuncName(t.p, obj))
	name := cFuncName(t.p, obj)
	if t.instName != "" {
		name = t.instName
	}
	return fmt.Sprintf("%s %s(%s)", ret, name, strings.Join(params, ", ")), sig
}

var multiRets = map[string]string{}

func (t *Tx) retTypeOf(res *types.Tuple, fname string) string {
	switch res.Len() {
	case 0:
		return "void"
	case 1:
		return t.ctype(res.At(0).Type())
	}
	var parts []string
	for i := 0; i < res.Len(); i++ {
		parts = append(parts, mangle(res.At(i).Type()))
	}
	name := "Ret_" + strings.Join(parts, "__")
	if len(name) > 80 {
		h := uint32(2166136261)
		for i := 0; i < len(name); i++ {
			h = (h ^ uint32(name[i])) * 16777619
		}
		name = fmt.Sprintf("Ret_%s_%08x", name[4:40], h)
	}
	var f []string
	for i := 0; i < res.Len(); i++ {
		f = append(f, fmt.Sprintf("%s r%d;", t.ctype(res.At(i).Type()), i))
	}
	multiRets[name] = "typedef struct { " + strings.Join(f, " ") + " } " + name + ";"
	return name
}

func (t *Tx) translateFunc(fd *ast.FuncDecl) string {
	t.out = &bytes.Buffer{}
	t.fn = fd
	t.labels = map[string]bool{}
	hdr, sig := t.funcHeader(fd)
	t.results = sig.Results()
	obj := t.p.info.Defs[fd.Name].(*types.Func)
	t.retType = t.retTypeOf(sig.Results(), cFuncName(t.p, obj))
	t.w("%s {", hdr)
	t.ind++
	t.heap, t.stackBox, t.nonEsc = heapVarsOf(t.p, fd)
	if fd.Body != nil && funcUsesDefer(fd.Body) {
		t.w("%s", deferMarker)
	}
	t.heapParams(sig)
	// named results
	t.namedRes = nil
	for i := 0; i < sig.Results().Len(); i++ {
		v := sig.Results().At(i)
		if v.Name() != "" && v.Name() != "_" {
			t.w("%s", t.declObj(v, t.ctype(v.Type()), "{0}"))
			t.namedRes = append(t.namedRes, t.varRef(v))
		}
	}
	if fd.Body != nil {
		for _, s := range fd.Body.List {
			t.stmt(s)
		}
	}
	t.runDefers()
	if len(t.namedRes) > 0 {
		t.w("%s", t.returnNamed())
	}
	t.ind--
	t.w("}")
	return t.finishDefers(t.out.String())
}

func (t *Tx) returnNamed() string {
	if len(t.namedRes) == 1 {
		return "return " + t.namedRes[0] + ";"
	}
	return fmt.Sprintf("return (%s){%s};", t.retType, strings.Join(t.namedRes, ", "))
}

// ---------------------------------------------------------------- statements

func (t *Tx) block(b *ast.BlockStmt) {
	t.w("{")
	t.ind++
	for _, s := range b.List {
		t.stmt(s)
	}
	t.ind--
	t.w("}")
}

func (t *Tx) stmt(s ast.Stmt) {
	switch s := s.(type) {
	case *ast.ExprStmt:
		t.w("%s;", t.expr(s.X))
	case *ast.AssignStmt:
		t.assign(s)
	case *ast.IncDecStmt:
		t.w("%s%s;", t.lvalue(s.X), s.Tok.String())
	case *ast.ReturnStmt:
		t.ret(s)
	case *ast.BlockStmt:
		t.block(s)
	case *ast.IfStmt:
		t.ifStmt(s, false)
	case *ast.ForStmt:
		t.forStmt(s, "")
	case *ast.RangeStmt:
		t.rangeStmt(s, "")
	case *ast.SwitchStmt:
		t.switchStmt(s, "")
	case *ast.TypeSwitchStmt:
		t.typeSwitch(s)
	case *ast.BranchStmt:
		switch s.Tok {
		case token.BREAK:
			if s.Label != nil {
				t.w("goto %s_break;", s.Label.Name)
			} else {
				t.w("break;")
			}
		case token.CONTINUE:
			if s.Label != nil {
				t.w("goto %s_continue;", s.Label.Name)
			} else if t.inSwitch > 0 && len(t.loopCont) > 0 {
				t.w("goto %s;", t.loopCont[len(t.loopCont)-1]) // continue from inside a (do-while) switch
			} else {
				t.w("continue;")
			}
		case token.GOTO:
			t.w("goto %s;", s.Label.Name)
		case token.FALLTHROUGH:
			t.w("/* fallthrough */")
		}
	case *ast.LabeledStmt:
		t.labeled(s)
	case *ast.DeclStmt:
		gd := s.Decl.(*ast.GenDecl)
		for _, spec := range gd.Specs {
			switch sp := spec.(type) {
			case *ast.ValueSpec:
				if len(sp.Names) > 1 && len(sp.Values) == 1 {
					// var a, b = f(): tuple-valued initializer
					tm := t.newTmp()
					t.w("%s %s = %s;", t.callRetType(sp.Values[0]), tm, t.expr(sp.Values[0]))
					for i, n := range sp.Names {
						if obj := t.p.info.Defs[n]; obj != nil && n.Name != "_" {
							t.w("%s", t.declObj(obj, t.ctype(obj.Type()), fmt.Sprintf("%s.r%d", tm, i)))
						}
					}
					continue
				}
				for i, n := range sp.Names {
					obj := t.p.info.Defs[n]
					if obj == nil {
						continue
					}
					if _, isConst := obj.(*types.Const); isConst {
						continue // folded at use sites
					}
					ty := t.ctype(obj.Type())
					if len(sp.Values) > i {
						val := t.exprConv(sp.Values[i], obj.Type())
						if usesName(sp.Values[i], n.Name) { // var x = f(x): outer x
							tm := t.newTmp()
							t.w("%s %s = %s;", ty, tm, val)
							val = tm
						}
						t.w("%s", t.declObj(obj, ty, val))
					} else {
						t.w("%s", t.declObj(obj, ty, zeroValue(obj.Type())))
					}
				}
			case *ast.TypeSpec:
				// hoisted to the header (typePreamble); referencing it registers it
				if obj := t.p.info.Defs[sp.Name]; obj != nil {
					cbase(obj.Type())
				}
			}
		}
	case *ast.DeferStmt:
		t.deferStmt(s)
	case *ast.SelectStmt:
		t.selectStmt(s)
	case *ast.SendStmt:
		t.sendStmt(s)
	case *ast.GoStmt:
		t.goStmt(s)
	case *ast.EmptyStmt:
	default:
		t.w("%s;", t.TODO(s, fmt.Sprintf("stmt %T", s)))
	}
}

func zeroValue(ty types.Type) string {
	switch u := ty.Underlying().(type) {
	case *types.Basic:
		if u.Info()&types.IsString != 0 {
			return "(GoString){0}"
		}
		if u.Kind() == types.Bool {
			return "false"
		}
		return "0"
	case *types.Pointer, *types.Chan:
		return "NULL"
	case *types.Signature:
		return "(GoFunc){0}"
	}
	return "{0}"
}

func (t *Tx) labeled(s *ast.LabeledStmt) {
	name := s.Label.Name
	switch st := s.Stmt.(type) {
	case *ast.ForStmt:
		t.forStmt(st, name)
		t.w("%s_break:;", name)
	case *ast.RangeStmt:
		t.rangeStmt(st, name)
		t.w("%s_break:;", name)
	case *ast.SwitchStmt:
		t.switchStmt(st, name)
		t.w("%s_break:;", name)
	default:
		t.w("%s:;", name)
		t.stmt(s.Stmt)
	}
}

func (t *Tx) ret(s *ast.ReturnStmt) {
	if len(t.defers) > 0 {
		// results are evaluated before the deferred calls run
		t.w("{")
		t.ind++
		if len(s.Results) > 0 {
			rv := t.newTmp()
			var val string
			switch {
			case t.results.Len() == 1:
				val = t.exprConv(s.Results[0], t.results.At(0).Type())
			case len(s.Results) == 1:
				val = t.expr(s.Results[0])
			default:
				var vs []string
				for i, r := range s.Results {
					vs = append(vs, t.exprConv(r, t.results.At(i).Type()))
				}
				val = fmt.Sprintf("(%s){%s}", t.retType, strings.Join(vs, ", "))
			}
			t.w("%s %s = %s;", t.retType, rv, val)
			t.runDefers()
			t.w("return %s;", rv)
		} else {
			t.runDefers()
			if len(t.namedRes) > 0 {
				t.w("%s", t.returnNamed())
			} else {
				t.w("return;")
			}
		}
		t.ind--
		t.w("}")
		return
	}
	if len(s.Results) == 0 {
		if len(t.namedRes) > 0 {
			t.w("%s", t.returnNamed())
		} else {
			t.w("return;")
		}
		return
	}
	if t.results.Len() == 1 {
		t.w("return %s;", t.exprConv(s.Results[0], t.results.At(0).Type()))
		return
	}
	if len(s.Results) == 1 && t.results.Len() > 1 {
		// return f() with multi-value f
		v := t.expr(s.Results[0])
		if strings.HasPrefix(v, "TODO(") {
			v = "(" + t.retType + "){0} /* untranslated call */"
		}
		t.w("return %s;", v)
		return
	}
	var vs []string
	for i, r := range s.Results {
		vs = append(vs, t.exprConv(r, t.results.At(i).Type()))
	}
	t.w("return (%s){%s};", t.retType, strings.Join(vs, ", "))
}

func (t *Tx) ifStmt(s *ast.IfStmt, isElse bool) {
	if s.Init != nil {
		t.w("{")
		t.ind++
		t.stmt(s.Init)
	}
	t.w("if (%s) {", t.cond(s.Cond))
	t.ind++
	for _, st := range s.Body.List {
		t.stmt(st)
	}
	t.ind--
	switch e := s.Else.(type) {
	case nil:
		t.w("}")
	case *ast.BlockStmt:
		t.w("} else {")
		t.ind++
		for _, st := range e.List {
			t.stmt(st)
		}
		t.ind--
		t.w("}")
	case *ast.IfStmt:
		t.w("} else {")
		t.ind++
		t.ifStmt(e, true)
		t.ind--
		t.w("}")
	}
	if s.Init != nil {
		t.ind--
		t.w("}")
	}
}

func (t *Tx) cond(e ast.Expr) string {
	s := t.expr(e)
	if strings.HasPrefix(s, "(") && strings.HasSuffix(s, ")") && balanced(s[1:len(s)-1]) {
		return s[1 : len(s)-1]
	}
	return s
}

func balanced(s string) bool {
	d := 0
	for _, c := range s {
		if c == '(' {
			d++
		} else if c == ')' {
			d--
			if d < 0 {
				return false
			}
		}
	}
	return d == 0
}

func (t *Tx) forStmt(s *ast.ForStmt, label string) {
	t.w("{")
	t.ind++
	if s.Init != nil {
		t.stmt(s.Init)
	}
	cond := "1"
	if s.Cond != nil {
		cond = t.cond(s.Cond)
	}
	// post statement rendered inline when simple
	post := ""
	if s.Post != nil {
		post = t.simpleStmtInline(s.Post)
	}
	t.w("for (; %s; %s) {", cond, post)
	t.ind++
	cl := t.enterLoop()
	for _, st := range s.Body.List {
		t.stmt(st)
	}
	t.leaveLoop(cl)
	if label != "" {
		t.w("%s_continue:;", label)
	}
	t.ind--
	t.w("}")
	t.ind--
	t.w("}")
}

func (t *Tx) simpleStmtInline(s ast.Stmt) string {
	switch s := s.(type) {
	case *ast.IncDecStmt:
		return t.lvalue(s.X) + s.Tok.String()
	case *ast.AssignStmt:
		if len(s.Lhs) == 1 && len(s.Rhs) == 1 && s.Tok != token.DEFINE {
			return t.assignExpr(s.Lhs[0], s.Tok, s.Rhs[0])
		}
		if len(s.Lhs) == len(s.Rhs) && s.Tok == token.ASSIGN {
			// parallel assignment i, j = a, b (rare in post statements)
			var parts []string
			for i := range s.Lhs {
				parts = append(parts, t.assignExpr(s.Lhs[i], token.ASSIGN, s.Rhs[i]))
			}
			return strings.Join(parts, ", ")
		}
	case *ast.ExprStmt:
		return t.expr(s.X)
	}
	return t.TODO(s, "post stmt")
}

func (t *Tx) rangeStmt(s *ast.RangeStmt, label string) {
	xt := t.typeOf(s.X)
	if xt == nil {
		t.w("%s;", t.TODO(s, "range: unknown type"))
		return
	}
	key, val := "", ""
	define := s.Tok == token.DEFINE
	var keyObj, valObj types.Object
	if s.Key != nil {
		if id, ok := s.Key.(*ast.Ident); !ok || id.Name != "_" {
			key = t.expr(s.Key)
			if define {
				keyObj = t.p.info.Defs[id]
			}
		}
	}
	if s.Value != nil {
		if id, ok := s.Value.(*ast.Ident); !ok || id.Name != "_" {
			val = t.expr(s.Value)
			if define {
				valObj = t.p.info.Defs[s.Value.(*ast.Ident)]
			}
		}
	}
	decl := func(o types.Object, ctype, v string) string {
		if o == nil {
			return fmt.Sprintf("%s = %s;", "/*?*/_", v)
		}
		return t.declObj(o, ctype, v)
	}
	idx := t.newTmp()
	t.w("{")
	t.ind++
	switch u := xt.Underlying().(type) {
	case *types.Slice, *types.Array, *types.Pointer:
		var elem types.Type
		lenExpr := ""
		x := t.expr(s.X)
		access := ""
		switch u2 := u.(type) {
		case *types.Slice:
			elem = u2.Elem()
			xs := t.newTmp()
			t.w("%s %s = %s;", t.ctype(xt), xs, x)
			lenExpr = xs + ".len"
			access = xs + ".p[" + idx + "]"
		case *types.Array:
			elem = u2.Elem()
			lenExpr = fmt.Sprint(u2.Len())
			access = "(" + x + ").v[" + idx + "]"
		case *types.Pointer:
			a := u2.Elem().Underlying().(*types.Array)
			elem = a.Elem()
			lenExpr = fmt.Sprint(a.Len())
			access = "(" + x + ")->v[" + idx + "]"
		}
		t.w("for (int %s = 0; %s < %s; %s++) {", idx, idx, lenExpr, idx)
		t.ind++
		if key != "" {
			if define {
				t.w("%s", decl(keyObj, "int", idx))
			} else {
				t.w("%s = %s;", key, idx)
			}
		}
		if val != "" {
			if define {
				t.w("%s", decl(valObj, t.ctype(elem), access))
			} else {
				t.w("%s = %s;", val, access)
			}
		}
	case *types.Basic:
		if u.Info()&types.IsString != 0 {
			xs := t.newTmp()
			t.w("GoString %s = %s;", xs, t.expr(s.X))
			w := t.newTmp()
			r := t.newTmp()
			t.w("for (int %s = 0, %s = 0; %s < %s.len; %s += %s) {", idx, w, idx, xs, idx, w)
			t.ind++
			t.w("i32 %s; %s = go_decode_rune(%s, %s, &%s);", r, w, xs, idx, r)
			if key != "" {
				if define {
					t.w("%s", decl(keyObj, "int", idx))
				} else {
					t.w("%s = %s;", key, idx)
				}
			}
			if val != "" {
				if define {
					t.w("%s", decl(valObj, "i32", r))
				} else {
					t.w("%s = %s;", val, r)
				}
			}
		} else {
			// range over int
			t.w("for (int %s = 0; %s < %s; %s++) {", idx, idx, t.expr(s.X), idx)
			t.ind++
			if key != "" {
				if define {
					t.w("%s", decl(keyObj, "int", idx))
				} else {
					t.w("%s = %s;", key, idx)
				}
			}
		}
	case *types.Map:
		mt := cbase(xt)
		it := t.newTmp()
		t.w("GoMapIter %s = gomap_iter((GoMap *)%s);", it, t.mapAddr(s.X))
		t.w("while (gomap_next(&%s)) {", it)
		t.ind++
		if key != "" {
			kv := fmt.Sprintf("*(%s *)%s.key", t.ctype(u.Key()), it)
			if define {
				t.w("%s", decl(keyObj, t.ctype(u.Key()), kv))
			} else {
				t.w("%s = %s;", key, kv)
			}
		}
		if val != "" {
			vv := fmt.Sprintf("*(%s *)%s.val", t.ctype(u.Elem()), it)
			if define {
				t.w("%s", decl(valObj, t.ctype(u.Elem()), vv))
			} else {
				t.w("%s = %s;", val, vv)
			}
		}
		_ = mt
	case *types.Chan:
		cv, tv, et := t.newTmp(), t.newTmp(), t.ctype(u.Elem())
		t.w("GoChan *%s = %s;", cv, t.expr(s.X))
		t.w("for (;;) {")
		t.ind++
		t.w("%s %s; if (!go_chan_recv(%s, &%s)) break;", et, tv, cv, tv)
		if key != "" { // for v := range ch: the element is the "key"
			if define {
				t.w("%s", decl(keyObj, et, tv))
			} else {
				t.w("%s = %s;", key, tv)
			}
		}
	default:
		t.w("%s;", t.TODO(s, "range kind"))
		t.ind--
		t.w("}")
		return
	}
	cl := t.enterLoop()
	for _, st := range s.Body.List {
		t.stmt(st)
	}
	t.leaveLoop(cl)
	if label != "" {
		t.w("%s_continue:;", label)
	}
	t.ind--
	t.w("}")
	t.ind--
	t.w("}")
}

type loopSave struct {
	inSwitch int
	label    string
}

func (t *Tx) enterLoop() loopSave {
	t.tmp++
	l := fmt.Sprintf("_cont%d", t.tmp)
	s := loopSave{t.inSwitch, l}
	t.loopCont = append(t.loopCont, l)
	t.inSwitch = 0
	return s
}

func (t *Tx) leaveLoop(s loopSave) {
	t.w("%s:;", s.label)
	t.loopCont = t.loopCont[:len(t.loopCont)-1]
	t.inSwitch = s.inSwitch
}

func (t *Tx) switchStmt(s *ast.SwitchStmt, label string) {
	t.w("{")
	t.ind++
	if s.Init != nil {
		t.stmt(s.Init)
	}
	tag := ""
	var tagType types.Type
	if s.Tag != nil {
		tag = t.newTmp()
		tagType = t.typeOf(s.Tag)
		t.w("%s %s = %s;", t.ctype(tagType), tag, t.expr(s.Tag))
	}
	// Emit as a do { if-chain } while(0) so that "break" leaves the switch.
	t.w("do {")
	t.ind++
	t.inSwitch++
	defer func() { t.inSwitch-- }()
	clauses := s.Body.List
	var deflt *ast.CaseClause
	first := true
	for ci, cc := range clauses {
		c := cc.(*ast.CaseClause)
		if c.List == nil {
			deflt = c
			continue
		}
		var conds []string
		for _, e := range c.List {
			if tag != "" {
				conds = append(conds, t.eqExpr(tag, tagType, e))
			} else {
				conds = append(conds, "("+t.cond(e)+")")
			}
		}
		kw := "if"
		if !first {
			kw = "} else if"
		}
		first = false
		t.w("%s (%s) {", kw, strings.Join(conds, " || "))
		t.ind++
		t.caseBody(c, clauses, ci)
		t.ind--
	}
	if deflt != nil {
		if first {
			t.w("{")
		} else {
			t.w("} else {")
		}
		t.ind++
		for ci, cc := range clauses {
			if cc == deflt {
				t.caseBody(deflt, clauses, ci)
			}
		}
		t.ind--
		t.w("}")
	} else if !first {
		t.w("}")
	}
	t.ind--
	t.w("} while (0);")
	if label != "" {
		t.w("%s_continue:;", label)
	}
	t.ind--
	t.w("}")
}

func (t *Tx) caseBody(c *ast.CaseClause, all []ast.Stmt, ci int) {
	for _, st := range c.Body {
		t.stmt(st)
	}
	// fallthrough: inline the next clause body
	if n := len(c.Body); n > 0 {
		if b, ok := c.Body[n-1].(*ast.BranchStmt); ok && b.Tok == token.FALLTHROUGH && ci+1 < len(all) {
			t.caseBody(all[ci+1].(*ast.CaseClause), all, ci+1)
		}
	}
}

func (t *Tx) eqExpr(tag string, tagType types.Type, e ast.Expr) string {
	if isString(tagType) {
		return "go_streq(" + tag + ", " + t.expr(e) + ")"
	}
	switch tagType.Underlying().(type) {
	case *types.Struct, *types.Array:
		return eqFuncName(tagType) + "(" + tag + ", " + t.exprConv(e, tagType) + ")"
	case *types.Interface:
		return "go_iface_eq(" + tag + ", " + t.exprConv(e, tagType) + ")"
	}
	return "(" + tag + " == " + t.exprConv(e, tagType) + ")"
}

// ---------------------------------------------------------------- assignment

func (t *Tx) assign(s *ast.AssignStmt) {
	if len(s.Lhs) == len(s.Rhs) {
		if len(s.Lhs) == 1 {
			lhs := s.Lhs[0]
			if s.Tok == token.DEFINE {
				id := lhs.(*ast.Ident)
				if id.Name == "_" {
					t.w("(void)(%s);", t.expr(s.Rhs[0]))
					return
				}
				obj := t.p.info.Defs[id]
				if obj == nil { // redeclared: plain assignment
					t.w("%s = %s;", t.expr(id), t.exprConv(s.Rhs[0], t.typeOf(id)))
					return
				}
				val := t.exprConv(s.Rhs[0], obj.Type())
				if usesName(s.Rhs[0], id.Name) {
					// x := f(x): Go's rhs sees the outer x, C's initializer the new one
					tm := t.newTmp()
					t.w("%s %s = %s;", t.ctype(obj.Type()), tm, val)
					val = tm
				}
				t.w("%s", t.declObj(obj, t.ctype(obj.Type()), val))
				return
			}
			t.w("%s;", t.assignExpr(lhs, s.Tok, s.Rhs[0]))
			return
		}
		// parallel assignment: evaluate all rhs first
		var tmps []string
		for i, r := range s.Rhs {
			ty := t.typeOf(s.Lhs[i])
			if ty == nil {
				ty = t.typeOf(r)
			}
			tm := t.newTmp()
			t.w("%s %s = %s;", t.ctype(ty), tm, t.exprConv(r, ty))
			tmps = append(tmps, tm)
		}
		for i, l := range s.Lhs {
			if id, ok := l.(*ast.Ident); ok && id.Name == "_" {
				continue
			}
			if s.Tok == token.DEFINE {
				id := l.(*ast.Ident)
				if obj := t.p.info.Defs[id]; obj != nil {
					t.w("%s", t.declObj(obj, t.ctype(obj.Type()), tmps[i]))
					continue
				}
			}
			t.w("%s = %s;", t.lvalue(l), tmps[i])
		}
		return
	}
	// multi-value: a, b := f()  /  v, ok := m[k]  /  v, ok := x.(T)
	if len(s.Rhs) == 1 && len(s.Lhs) == 2 {
		switch r := s.Rhs[0].(type) {
		case *ast.IndexExpr:
			if mt, ok := t.typeOf(r.X).Underlying().(*types.Map); ok {
				ok2 := t.newTmp()
				val := t.newTmp()
				t.w("bool %s; %s %s = %s;", ok2, t.ctype(mt.Elem()), val, t.mapGet(r.X, r.Index, mt, "&"+ok2))
				t.defOrSet(s.Lhs[0], s.Tok, val, mt.Elem())
				t.defOrSet(s.Lhs[1], s.Tok, ok2, types.Typ[types.Bool])
				return
			}
		case *ast.UnaryExpr:
			if r.Op == token.ARROW { // v, ok := <-ch
				el := chanElem(t.typeOf(r.X))
				val, okv := t.newTmp(), t.newTmp()
				t.w("%s %s; bool %s = go_chan_recv(%s, &%s);", t.ctype(el), val, okv, t.expr(r.X), val)
				t.defOrSet(s.Lhs[0], s.Tok, val, el)
				t.defOrSet(s.Lhs[1], s.Tok, okv, types.Typ[types.Bool])
				return
			}
		case *ast.TypeAssertExpr:
			ta := s.Rhs[0].(*ast.TypeAssertExpr)
			ty := substT(t.p.info.TypeOf(ta.Type))
			if ty == nil || ty.Underlying() == nil || types.TypeString(ty, qual) == "invalid type" {
				t.w("/* type assertion to an unresolved type at %s */", shortFile(t.p.fset.Position(s.Pos()).Filename))
				if id, ok := s.Lhs[0].(*ast.Ident); ok && id.Name != "_" && s.Tok == token.DEFINE {
					t.w("GoOpaque %s = {{0}};", cVarName(id.Name))
				}
				if id, ok := s.Lhs[1].(*ast.Ident); ok && id.Name != "_" {
					if s.Tok == token.DEFINE {
						t.w("bool %s = false;", cVarName(id.Name))
					} else {
						t.w("%s = false;", t.expr(id))
					}
				}
				return
			}
			xv := t.newTmp()
			t.w("GoIface %s = %s;", xv, t.expr(ta.X))
			var cond, val string
			if isIface(ty) {
				cond = t.implementsCond(xv, ty)
				val = xv
			} else {
				cond = fmt.Sprintf("%s.type == %d", xv, typeID(ty))
				z := t.newTmp()
				t.w("%s %s; memset(&%s, 0, sizeof %s); if (%s) %s = %s;", cbase(ty), z, z, z, cond, z, t.unbox(xv, ty))
				val = z
			}
			okv := t.newTmp()
			t.w("bool %s = %s;", okv, cond)
			t.defOrSet(s.Lhs[0], s.Tok, val, ty)
			t.defOrSet(s.Lhs[1], s.Tok, okv, types.Typ[types.Bool])
			return
		}
	}
	if len(s.Rhs) == 1 {
		rt := t.typeOf(s.Rhs[0])
		tup, ok := rt.(*types.Tuple)
		if ok {
			tm := t.newTmp()
			t.w("%s %s = %s;", t.callRetType(s.Rhs[0]), tm, t.expr(s.Rhs[0]))
			for i, l := range s.Lhs {
				t.defOrSet(l, s.Tok, fmt.Sprintf("%s.r%d", tm, i), tup.At(i).Type())
			}
			return
		}
	}
	t.w("%s;", t.TODO(s, "assign form"))
}

func (t *Tx) callRetType(e ast.Expr) string {
	if tup, ok := t.typeOf(e).(*types.Tuple); ok {
		return t.retTypeOf(tup, "")
	}
	return "/*?*/RetUnknown"
}

func (t *Tx) defOrSet(l ast.Expr, tok token.Token, val string, ty types.Type) {
	if id, ok := l.(*ast.Ident); ok {
		if id.Name == "_" {
			return
		}
		if tok == token.DEFINE {
			if obj := t.p.info.Defs[id]; obj != nil {
				t.w("%s", t.declObj(obj, t.ctype(obj.Type()), val))
				return
			}
		}
	}
	t.w("%s = %s;", t.lvalue(l), val)
}

func (t *Tx) lvalue(e ast.Expr) string {
	if ix, ok := e.(*ast.IndexExpr); ok {
		if mt, isMap := t.typeOf(ix.X).Underlying().(*types.Map); isMap {
			return fmt.Sprintf("(*%s_ptr(%s, %s))", cbase(mt), t.mapAddr(ix.X), t.exprConv(ix.Index, mt.Key()))
		}
	}
	return t.expr(e)
}

func (t *Tx) assignExpr(lhs ast.Expr, tok token.Token, rhs ast.Expr) string {
	lt := t.typeOf(lhs)
	// map store
	if ix, ok := lhs.(*ast.IndexExpr); ok {
		if mt, isMap := t.typeOf(ix.X).Underlying().(*types.Map); isMap {
			val := t.exprConv(rhs, mt.Elem())
			if tok != token.ASSIGN {
				op := strings.TrimSuffix(tok.String(), "=")
				val = fmt.Sprintf("(%s %s %s)", t.mapGet(ix.X, ix.Index, mt, "NULL"), op, val)
			}
			return t.mapSet(ix.X, ix.Index, mt, val)
		}
	}
	if id, ok := lhs.(*ast.Ident); ok && id.Name == "_" {
		return "(void)(" + t.expr(rhs) + ")"
	}
	l := t.expr(lhs)
	switch tok {
	case token.ASSIGN:
		return l + " = " + t.exprConv(rhs, lt)
	case token.ADD_ASSIGN:
		if isString(lt) {
			return l + " = go_strcat(" + l + ", " + t.expr(rhs) + ")"
		}
	case token.SHL_ASSIGN, token.SHR_ASSIGN:
		op := "shl"
		if tok == token.SHR_ASSIGN {
			op = "shr"
		}
		return fmt.Sprintf("%s = %s", l, t.shift(op, l, lt, rhs))
	case token.AND_NOT_ASSIGN:
		return l + " &= ~(" + t.exprConv(rhs, lt) + ")"
	}
	return l + " " + tok.String() + " " + t.exprConv(rhs, lt)
}

// usesName reports whether e mentions an identifier spelled name.
func usesName(e ast.Expr, name string) bool {
	found := false
	ast.Inspect(e, func(n ast.Node) bool {
		if id, ok := n.(*ast.Ident); ok && id.Name == name {
			found = true
		}
		return !found
	})
	return found
}
