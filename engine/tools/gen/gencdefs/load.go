package main

import (
	"go/ast"
	"go/build"
	"go/importer"
	"go/parser"
	"go/token"
	"go/types"
	"os"
	"path/filepath"
	"strings"
)

// Packages whose types get C layouts besides main: those gotoc translates
// (GOTOC_PKGS) and the cgo bindings (GOTOC_BIND_PKGS). Their type names
// carry the package name (lua_LState, sdl_Window), as in gotoc.
var ownPkgs = map[*types.Package]bool{}

// typeByName: every named type gen_types.h may define, by C name.
var typeByName = map[string]types.Type{}

type srcImporter struct {
	fset     *token.FileSet
	want     map[string]bool
	auto     bool            // GOTOC_PKGS=auto,...: every third-party import (minus GOTOC_SKIP_PKGS)
	skip     map[string]bool
	stdWant  map[string]bool // GOTOC_STD_PKGS: standard-library packages to translate
	bind     map[string]bool
	fallback types.ImporterFrom
	done     map[string]*types.Package
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
	var files []*ast.File
	names := bp.GoFiles
	if im.bind[path] {
		names = append(names, bp.CgoFiles...)
	}
	for _, f := range names {
		if af, err := parser.ParseFile(im.fset, filepath.Join(bp.Dir, f), nil, 0); err == nil {
			files = append(files, af)
		}
	}
	conf := &types.Config{FakeImportC: true, Importer: im, Error: func(error) {}}
	binfo := &types.Info{Defs: map[*ast.Ident]types.Object{}}
	if im.bind[path] {
		rewriteBindTypes(files)
	}
	pk, _ := conf.Check(path, im.fset, files, binfo)
	if im.bind[path] {
		recordBindC(files, binfo)
	}
	im.done[path] = pk
	registerPkg(pk)
	ownPkgs[pk] = true
	return pk, nil
}

func newImporter(fset *token.FileSet) *srcImporter {
	im := &srcImporter{fset: fset, want: map[string]bool{}, bind: map[string]bool{}, done: map[string]*types.Package{},
		skip: pkgSet("GOTOC_SKIP_PKGS"), stdWant: pkgSet("GOTOC_STD_PKGS"),
		fallback: importer.ForCompiler(fset, "source", nil).(types.ImporterFrom)}
	for _, p := range strings.Split(os.Getenv("GOTOC_PKGS"), ",") {
		if p == "auto" {
			im.auto = true
		} else if p != "" {
			im.want[p] = true
		}
	}
	for _, p := range strings.Split(os.Getenv("GOTOC_BIND_PKGS"), ",") {
		if p != "" {
			im.bind[p] = true
		}
	}
	return im
}

// C name of a named type of main or of another own package.
func ownTypeName(n *types.Named) string {
	name := n.Obj().Name()
	if pk := n.Obj().Pkg(); pk != pkg {
		name = pkgPrefix(pk) + "_" + name
	}
	if n.TypeArgs().Len() > 0 {
		var parts []string
		for i := 0; i < n.TypeArgs().Len(); i++ {
			parts = append(parts, mangle(n.TypeArgs().At(i)))
		}
		name += "_" + strings.Join(parts, "_")
	}
	return name
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
