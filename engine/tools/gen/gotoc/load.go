package main

import (
	"go/ast"
	"go/build"
	"go/importer"
	"go/parser"
	"go/token"
	"go/types"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type Prog struct {
	fset  *token.FileSet
	files []*ast.File
	pkg   *types.Package
	info  *types.Info
	funcs map[string]*ast.FuncDecl // "Recv.Name" or "Name"; "pkg.Recv.Name" outside main
	own   map[*types.Package]bool  // packages translated besides main (GOTOC_PKGS)
	// initialisation order: packages in dependency order (main last) and
	// each package's init functions in source order
	pkgOrder  []*types.Package
	initFuncs map[*types.Package][]string
	// package variable initializers of every translated package (go/types
	// resets Info.InitOrder on each Check, so it is collected per package)
	initOrder []*types.Initializer
}

// ownPkgs: every translated package, main included (set by main()).
var ownPkgs = map[*types.Package]bool{}

func isOwn(pk *types.Package) bool { return pk != nil && ownPkgs[pk] }

// bindPkgs: cgo binding packages (GOTOC_BIND_PKGS). Their types get real C
// layouts like own types; their functions stay extern (hand-written C).
var bindPkgs = map[*types.Package]bool{}

func ownTypes(pk *types.Package) bool { return pk != nil && (ownPkgs[pk] || bindPkgs[pk]) }

// keyPrefix: function keys and C names of non-main packages carry the
// package name ("lua.LState.Push", lua_LState_Push).
func keyPrefix(pk *types.Package) string {
	if pk == nil || pk.Name() == "main" {
		return ""
	}
	return pk.Path() + "." // unique even for packages sharing a name
}

// srcImporter type-checks the GOTOC_PKGS packages from source into the
// shared types.Info (so their bodies can be translated) and leaves every
// other import to the default source importer.
type srcImporter struct {
	p        *Prog
	want     map[string]bool
	auto     bool            // GOTOC_PKGS=auto,...: every third-party import (minus GOTOC_SKIP_PKGS)
	skip     map[string]bool
	stdWant  map[string]bool // GOTOC_STD_PKGS: standard-library packages to translate
	bind     map[string]bool // GOTOC_BIND_PKGS: cgo bindings, typed but implemented in C
	fallback types.ImporterFrom
	done     map[string]*types.Package
	conf     *types.Config
}

func (im *srcImporter) Import(path string) (*types.Package, error) { return im.ImportFrom(path, "", 0) }

func (im *srcImporter) ImportFrom(path, dir string, mode types.ImportMode) (*types.Package, error) {
	if !im.wanted(path, dir) && !im.bind[path] {
		return im.fallback.ImportFrom(path, dir, mode)
	}
	if pk, ok := im.done[path]; ok {
		return pk, nil
	}
	ctx := build.Default
	ctx.CgoEnabled = im.bind[path]
	bp, err := ctx.Import(path, dir, 0)
	if err != nil {
		return nil, err
	}
	if im.bind[path] {
		// cgo binding: keep its cgo files for the Go-level signatures; the
		// functions themselves are implemented by hand in C (src/platform)
		var files []*ast.File
		for _, f := range append(bp.GoFiles, bp.CgoFiles...) {
			af, err := parser.ParseFile(im.p.fset, filepath.Join(bp.Dir, f), nil, 0)
			if err == nil {
				files = append(files, af)
			}
		}
		conf := &types.Config{FakeImportC: true, Importer: im, Error: im.conf.Error}
		binfo := &types.Info{Defs: map[*ast.Ident]types.Object{}}
		rewriteBindTypes(files)
		pk, _ := conf.Check(path, im.p.fset, files, binfo)
		recordBindC(files, binfo)
		im.done[path] = pk
		registerPkg(pk)
		bindPkgs[pk] = true
		return pk, nil
	}
	var files []*ast.File
	for _, f := range bp.GoFiles {
		af, err := parser.ParseFile(im.p.fset, filepath.Join(bp.Dir, f), nil, parser.ParseComments)
		if err == nil {
			files = append(files, af)
		}
	}
	pk, _ := im.conf.Check(path, im.p.fset, files, im.p.info)
	im.p.takeInitOrder()
	im.done[path] = pk
	registerPkg(pk)
	im.p.own[pk] = true
	im.p.pkgOrder = append(im.p.pkgOrder, pk)
	for _, f := range files {
		im.p.addFuncs(f, pk)
	}
	return pk, nil
}

func (p *Prog) addFuncs(f *ast.File, pk *types.Package) {
	for _, d := range f.Decls {
		fd, ok := d.(*ast.FuncDecl)
		if !ok {
			continue
		}
		if p.info.Defs[fd.Name] == nil {
			fmt.Fprintln(os.Stderr, "skipping undeclared func", funcKey(fd), p.fset.Position(fd.Pos()))
			continue
		}
		if fd.Name.Name == "init" && fd.Recv == nil {
			if pk.Name() == "main" {
				continue // main's init functions are platform setup (thread locking)
			}
			k := fmt.Sprintf("%sinit#%d", keyPrefix(pk), len(p.initFuncs[pk]))
			p.initFuncs[pk] = append(p.initFuncs[pk], k)
			p.funcs[k] = fd
			continue
		}
		p.funcs[keyPrefix(pk)+funcKey(fd)] = fd
	}
}

func load(dir string) *Prog {
	ctx := build.Default
	ctx.BuildTags = []string{"vita"}
	if t, ok := os.LookupEnv("GOTOC_TAGS"); ok { // build configuration (default: vita)
		ctx.BuildTags = strings.Fields(strings.ReplaceAll(t, ",", " "))
	}
	ctx.CgoEnabled = true // keep cgo files of the main package (FakeImportC)
	bp, _ := ctx.ImportDir(dir, 0)
	p := &Prog{fset: token.NewFileSet(), funcs: map[string]*ast.FuncDecl{}, own: map[*types.Package]bool{}, initFuncs: map[*types.Package][]string{}}
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
	errf := func(err error) {
		if f := os.Getenv("GOTOC_TYPE_ERRORS"); f != "" && strings.Contains(err.Error(), f) {
			fmt.Fprintln(os.Stderr, "type error:", err)
		}
	}
	im := &srcImporter{p: p, want: map[string]bool{}, bind: map[string]bool{}, done: map[string]*types.Package{},
		skip: pkgSet("GOTOC_SKIP_PKGS"), stdWant: pkgSet("GOTOC_STD_PKGS"),
		fallback: importer.ForCompiler(p.fset, "source", nil).(types.ImporterFrom)}
	for _, path := range strings.Split(os.Getenv("GOTOC_PKGS"), ",") {
		if path == "auto" {
			im.auto = true
		} else if path != "" {
			im.want[path] = true
		}
	}
	for _, path := range strings.Split(os.Getenv("GOTOC_BIND_PKGS"), ",") {
		if path != "" {
			im.bind[path] = true
		}
	}
	im.conf = &types.Config{Importer: im, Error: errf}
	conf := types.Config{FakeImportC: true, Importer: im, Error: errf}
	p.pkg, _ = conf.Check("main", p.fset, p.files, p.info)
	p.takeInitOrder()
	p.pkgOrder = append(p.pkgOrder, p.pkg)
	for _, f := range p.files {
		p.addFuncs(f, p.pkg)
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
	if ix, ok := t.(*ast.IndexListExpr); ok { // T[K, V]
		t = ix.X
	}
	return t.(*ast.Ident).Name + "." + fd.Name.Name
}

// gName: C name of an own package-level variable.
func gName(v *types.Var) string {
	if v.Pkg() != nil && v.Pkg().Name() != "main" {
		return "g_" + pkgPrefix(v.Pkg()) + "_" + v.Name()
	}
	return "g_" + v.Name()
}

// init functions: one C function per declaration, run by ik_init_globals.
var initNames = map[*types.Func]string{}
var initOrderFns []*types.Func

func initFuncName(fn *types.Func) string {
	if n, ok := initNames[fn]; ok {
		return n
	}
	pfx := "fn_"
	if fn.Pkg() != nil && fn.Pkg().Name() != "main" {
		pfx = pkgPrefix(fn.Pkg()) + "_"
	}
	n := fmt.Sprintf("%sinit_%d", pfx, len(initNames))
	initNames[fn] = n
	return n
}

func (im *srcImporter) wanted(path, dir string) bool {
	if im.want[path] || im.stdWant[path] {
		return true
	}
	if !im.auto || im.skip[path] || path == "C" || path == "unsafe" {
		return false
	}
	for sk := range im.skip { // prefix entries: "golang.org/x/exp/..."
		if strings.HasSuffix(sk, "/...") && strings.HasPrefix(path, strings.TrimSuffix(sk, "...")) {
			return false
		}
	}
	bp, err := build.Default.Import(path, dir, build.FindOnly)
	return err == nil && !bp.Goroot // third-party (module) package
}

func pkgSet(env string) map[string]bool {
	m := map[string]bool{}
	for _, p := range strings.Split(os.Getenv(env), ",") {
		if p = strings.TrimSpace(p); p != "" {
			m[p] = true
		}
	}
	return m
}

func (p *Prog) takeInitOrder() {
	p.initOrder = append(p.initOrder, p.info.InitOrder...)
	p.info.InitOrder = nil
}
