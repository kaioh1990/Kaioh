package main

import (
	"go/build"
	"go/types"
	"sort"
	"strconv"
)

// C name prefixes of non-main packages (lua_, sdl_, frame_, frame2_...).
// Translated packages whose name repeats get a number in import-path order;
// a translated package named like a standard one (github.com/pkg/errors)
// gets "_x" so it cannot collide with the hand-written std shims. gotoc and
// gencdefs load the same packages and compute the same prefixes.
var allPkgs = map[string]*types.Package{}
var pkgPfx map[*types.Package]string

func registerPkg(pk *types.Package) {
	if pk != nil {
		allPkgs[pk.Path()] = pk
		pkgPfx = nil
	}
}

func pkgPrefix(pk *types.Package) string {
	if pkgPfx == nil {
		pkgPfx = map[*types.Package]string{}
		var paths []string
		for p := range allPkgs {
			paths = append(paths, p)
		}
		sort.Strings(paths)
		used := map[string]int{}
		for _, p := range paths {
			pk := allPkgs[p]
			name := pk.Name()
			if bp, err := build.Default.Import(p, "", build.FindOnly); err == nil && !bp.Goroot && stdName(name) {
				name += "_x"
			}
			used[name]++
			if used[name] > 1 {
				name += strconv.Itoa(used[name])
			}
			pkgPfx[pk] = name
		}
	}
	if n, ok := pkgPfx[pk]; ok {
		return n
	}
	return pk.Name()
}

func stdName(n string) bool {
	switch n {
	case "errors", "io", "os", "fmt", "strings", "bytes", "bufio", "sort", "strconv", "math", "time", "sync",
		"image", "color", "draw", "png", "json", "binary", "utf8", "unicode", "bits", "filepath", "path", "regexp",
		"reflect", "runtime", "atomic", "list", "context", "net", "http", "url", "log", "flag", "hash", "crc32",
		"zlib", "flate", "base64", "hex", "rand", "cmp", "slices", "maps", "debug", "ioutil", "xml", "zip", "gob":
		return true
	}
	return false
}
