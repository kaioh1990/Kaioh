package main

import (
	"go/ast"
	"go/constant"
	"go/token"
	"go/types"
	"strings"
)

// cgo binding packages declare some types and constants directly as C ones
// (type Keycode C.SDL_Keycode, BUTTON_A = C.SDL_CONTROLLER_BUTTON_A); the Go
// type checker knows neither their layout nor their values. They map to
// the C declarations by name.
var bindCType = map[*types.TypeName]string{} // Go type -> C type name
var bindCConst = map[*types.Const]string{}   // Go constant -> C macro/enumerator

func cSel(e ast.Expr) string {
	for {
		switch x := e.(type) {
		case *ast.ParenExpr:
			e = x.X
			continue
		case *ast.CallExpr: // T(C.X) conversions
			if len(x.Args) == 1 {
				e = x.Args[0]
				continue
			}
		case *ast.SelectorExpr:
			if id, ok := x.X.(*ast.Ident); ok && id.Name == "C" {
				return x.Sel.Name
			}
		}
		return ""
	}
}

func recordBindC(files []*ast.File, info *types.Info) {
	for _, f := range files {
		for _, d := range f.Decls {
			gd, ok := d.(*ast.GenDecl)
			if !ok {
				continue
			}
			for _, sp := range gd.Specs {
				switch s := sp.(type) {
				case *ast.TypeSpec:
					n := cSel(s.Type)
					if n == "" {
						n = bindCTypeByName[s.Name.Name+"@"+f.Name.Name]
					}
					if n != "" {
						if tn, ok := info.Defs[s.Name].(*types.TypeName); ok {
							bindCType[tn] = n
						}
					}
				case *ast.ValueSpec:
					for i, nm := range s.Names {
						if i < len(s.Values) {
							if n := cSel(s.Values[i]); n != "" {
								if c, ok := info.Defs[nm].(*types.Const); ok {
									bindCConst[c] = n
								}
							}
						}
					}
				}
			}
		}
	}
}

// C layout of a binding type declared as a C type.
func bindCLayout(cname string) string {
	switch cname {
	case "SDL_Keycode", "SDL_JoystickID":
		return "i32"
	case "SDL_FingerID", "SDL_TouchID", "SDL_GestureID":
		return "i64"
	case "SDL_GLContext", "SDL_MetalView", "SDL_AudioCallback":
		return "void*"
	}
	if strings.HasPrefix(cname, "SDL_") {
		switch cname {
		case "SDL_Window", "SDL_Renderer", "SDL_Texture", "SDL_Cursor", "SDL_GameController", "SDL_Haptic",
			"SDL_Joystick", "SDL_RWops", "SDL_Sensor", "SDL_AudioStream", "SDL_GUID", "SDL_JoystickGUID",
			"SDL_GameControllerButtonBind":
			return "GoOpaque" // used through pointers (or never by value)
		}
		return "int" // enums
	}
	return "GoOpaque"
}

// bindTypedef: no separate typedef is needed any more - rewriteBindTypes
// gives C-declared binding types a Go layout before type checking.
func bindTypedef(n *types.Named) (string, bool) { return "", false }

// known: a constant value the type checker computed (binding constants
// defined in C are Unknown and are emitted by name instead).
func known(v constant.Value) bool { return v != nil && v.Kind() != constant.Unknown }

// rewriteBindTypes replaces `type X C.Y` declarations in binding sources by
// Go types with the C layout, before type checking, so conversions and
// arithmetic on them type-check like in the real cgo build.
func rewriteBindTypes(files []*ast.File) {
	for _, f := range files {
		for _, d := range f.Decls {
			gd, ok := d.(*ast.GenDecl)
			if !ok {
				continue
			}
			for _, sp := range gd.Specs {
				ts, ok := sp.(*ast.TypeSpec)
				if !ok {
					continue
				}
				cn := cSel(ts.Type)
				if cn == "" {
					continue
				}
				bindCTypeByName[ts.Name.Name+"@"+f.Name.Name] = cn
				switch bindCLayout(cn) {
				case "i32", "int":
					ts.Type = ast.NewIdent("int32")
				case "i64":
					ts.Type = ast.NewIdent("int64")
				case "void*":
					ts.Type = ast.NewIdent("uintptr")
				default: // opaque C struct, used through pointers
					ts.Type = &ast.StructType{Fields: &ast.FieldList{List: []*ast.Field{{
						Names: []*ast.Ident{ast.NewIdent("_")},
						Type:  &ast.ArrayType{Len: &ast.BasicLit{Kind: token.INT, Value: "32"}, Elt: ast.NewIdent("byte")},
					}}}}
				}
			}
		}
	}
}

var bindCTypeByName = map[string]string{}
