package main

// CvF native engine content cooker (ux0:data/CvF/vita_cook.txt).
//
// Right after a character is compiled (before any Vita-only bytecode
// transformation), the compiled states, functions, string pool, constants
// and [Data]/[Size]/[Velocity]/[Movement] values are written to
// cooked/<def path>.cvfc. The native C engine loads these files, so it runs
// exactly the bytecode Ikemen's own compiler produced.
//
// Format (little endian): see engine/docs/COOKED_FORMAT.md.

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"reflect"
	"sort"
	"strings"
)

var cookOn = -1

func cookEnabled() bool {
	if cookOn < 0 {
		cookOn = 0
		if FileExist("vita_cook.txt") != "" {
			cookOn = 1
		}
	}
	return cookOn == 1
}

type cookW struct{ b []byte }

func (w *cookW) u8(v uint8)   { w.b = append(w.b, v) }
func (w *cookW) u32(v uint32) { w.b = binary.LittleEndian.AppendUint32(w.b, v) }
func (w *cookW) i32(v int32)  { w.u32(uint32(v)) }
func (w *cookW) f64(v float64) {
	w.b = binary.LittleEndian.AppendUint64(w.b, math.Float64bits(v))
}
func (w *cookW) f32(v float32) { w.u32(math.Float32bits(v)) }
func (w *cookW) str(s string) {
	w.u32(uint32(len(s)))
	w.b = append(w.b, s...)
}
func (w *cookW) exp(e BytecodeExp) {
	w.u32(uint32(len(e)))
	for _, op := range e {
		w.b = append(w.b, byte(op))
	}
}
func (w *cookW) bool(v bool) {
	if v {
		w.u8(1)
	} else {
		w.u8(0)
	}
}

const (
	cookCtlBlock    = 0
	cookCtlSCB      = 1
	cookCtlCallFunc = 2
	cookCtlExpr     = 3
	cookCtlVarAsgn  = 4
	cookCtlBreak    = 5
	cookCtlContinue = 6
	cookCtlNull     = 7
	cookCtlUnknown  = 255
)

var scParamType = reflect.TypeOf(scParam{})

func (w *cookW) scb(params []scParam) {
	w.u32(uint32(len(params)))
	for _, p := range params {
		w.u8(p.id)
		w.u32(uint32(len(p.exp)))
		for _, e := range p.exp {
			w.exp(e)
		}
	}
}

func (w *cookW) ctrl(sc StateController) {
	switch v := sc.(type) {
	case *StateBlock:
		w.u8(cookCtlBlock)
		w.block(v)
		return
	case CallFunction:
		w.u8(cookCtlCallFunc)
		w.str(v.name)
		w.exp(v.arg)
		w.u32(uint32(len(v.ret)))
		w.b = append(w.b, v.ret...)
		w.i32(v.numArgs)
		return
	case StateExpr:
		w.u8(cookCtlExpr)
		w.exp(BytecodeExp(v))
		return
	case varAssign:
		w.u8(cookCtlVarAsgn)
		w.u8(v.vari)
		w.exp(v.be)
		return
	case LoopBreak:
		w.u8(cookCtlBreak)
		return
	case LoopContinue:
		w.u8(cookCtlContinue)
		return
	case NullStateController:
		w.u8(cookCtlNull)
		return
	}
	rv := reflect.ValueOf(sc)
	if rv.Kind() == reflect.Slice && rv.Type().Elem() == scParamType {
		w.u8(cookCtlSCB)
		name := strings.TrimPrefix(fmt.Sprintf("%T", sc), "main.")
		w.str(name)
		params := make([]scParam, rv.Len())
		for i := range params {
			params[i] = rv.Index(i).Interface().(scParam)
		}
		w.scb(params)
		return
	}
	w.u8(cookCtlUnknown)
	w.str(fmt.Sprintf("%T", sc))
}

func (w *cookW) block(b *StateBlock) {
	w.i32(b.persistent)
	w.i32(b.persistentIndex)
	w.bool(b.ignorehitpause)
	w.bool(b.ctrlsIgnorehitpause)
	w.exp(b.trigger)
	w.bool(b.loopBlock)
	w.bool(b.nestedInLoop)
	w.bool(b.forLoop)
	w.bool(b.forAssign)
	w.u8(b.forCtrlVar.vari)
	w.exp(b.forCtrlVar.be)
	for i := 0; i < 3; i++ {
		w.exp(b.forExpression[i])
	}
	w.i32(b.forBegin)
	w.i32(b.forEnd)
	w.i32(b.forIncrement)
	w.u32(uint32(len(b.ctrls)))
	for _, sc := range b.ctrls {
		w.ctrl(sc)
	}
	w.bool(b.elseBlock != nil)
	if b.elseBlock != nil {
		w.block(b.elseBlock)
	}
}

// structFields writes every numeric field of a struct (recursively, arrays
// flattened) as name=value pairs, so the C side can map them by name.
func (w *cookW) structFields(prefix string, v reflect.Value, out *[]string, vals *[]float64) {
	t := v.Type()
	for i := 0; i < v.NumField(); i++ {
		f := v.Field(i)
		name := prefix + t.Field(i).Name
		switch f.Kind() {
		case reflect.Struct:
			w.structFields(name+".", f, out, vals)
		case reflect.Array:
			for k := 0; k < f.Len(); k++ {
				e := f.Index(k)
				switch e.Kind() {
				case reflect.Float32, reflect.Float64:
					*out = append(*out, fmt.Sprintf("%s[%d]", name, k))
					*vals = append(*vals, e.Float())
				case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
					*out = append(*out, fmt.Sprintf("%s[%d]", name, k))
					*vals = append(*vals, float64(e.Int()))
				}
			}
		case reflect.Float32, reflect.Float64:
			*out = append(*out, name)
			*vals = append(*vals, f.Float())
		case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
			*out = append(*out, name)
			*vals = append(*vals, float64(f.Int()))
		case reflect.Uint8, reflect.Uint16, reflect.Uint32:
			*out = append(*out, name)
			*vals = append(*vals, float64(f.Uint()))
		case reflect.Bool:
			*out = append(*out, name)
			b := 0.0
			if f.Bool() {
				b = 1
			}
			*vals = append(*vals, b)
		}
	}
}

func (w *cookW) numericBlock(tag string, v any) {
	var names []string
	var vals []float64
	w.structFields("", reflect.ValueOf(v), &names, &vals)
	w.str(tag)
	w.u32(uint32(len(names)))
	for i := range names {
		w.str(names[i])
		w.f64(vals[i])
	}
}

func cookChar(cdef string, pn int, gi *CharGlobalInfo) {
	if !cookEnabled() {
		return
	}
	w := &cookW{}
	w.b = append(w.b, "CVFC"...)
	w.u32(1) // version
	w.str(cdef)
	w.str(gi.name)
	w.str(gi.displayname)
	w.str(gi.author)
	w.u32(uint32(gi.ikemenver[0]) | uint32(gi.ikemenver[1])<<16)
	w.u32(uint32(gi.ikemenver[2]))
	w.u32(uint32(gi.mugenver[0]) | uint32(gi.mugenver[1])<<16)
	w.i32(gi.localcoord[0])
	w.i32(gi.localcoord[1])
	w.f32(gi.rawlocalscl)
	w.i32(gi.attackBase)
	w.i32(gi.defenceBase)
	// String pool
	pool := sys.stringPool[pn].List
	w.u32(uint32(len(pool)))
	for _, s := range pool {
		w.str(s)
	}
	// Constants
	ckeys := make([]string, 0, len(gi.constants))
	for k := range gi.constants {
		ckeys = append(ckeys, k)
	}
	sort.Strings(ckeys)
	w.u32(uint32(len(ckeys)))
	for _, k := range ckeys {
		tv := gi.constants[k]
		w.str(k)
		w.u8(uint8(tv.Type))
		w.f64(tv.Num)
		w.str(tv.Str)
	}
	// Character data sections
	w.numericBlock("data", gi.data)
	w.numericBlock("velocity", gi.velocity)
	w.numericBlock("movement", gi.movement)
	// Functions
	fkeys := make([]string, 0, len(gi.callFuncs))
	for k := range gi.callFuncs {
		fkeys = append(fkeys, k)
	}
	sort.Strings(fkeys)
	w.u32(uint32(len(fkeys)))
	for _, k := range fkeys {
		f := gi.callFuncs[k]
		w.str(k)
		w.i32(f.numVars)
		w.i32(f.numArgs)
		w.i32(f.numRets)
		w.u32(uint32(len(f.ctrls)))
		for _, sc := range f.ctrls {
			w.ctrl(sc)
		}
	}
	// States
	skeys := make([]int, 0, len(gi.states))
	for k := range gi.states {
		skeys = append(skeys, int(k))
	}
	sort.Ints(skeys)
	w.u32(uint32(len(skeys)))
	for _, k := range skeys {
		sb := gi.states[int32(k)]
		w.i32(int32(k))
		w.i32(int32(sb.stateType))
		w.exp(sb.stateTypeExp)
		w.i32(int32(sb.moveType))
		w.exp(sb.moveTypeExp)
		w.i32(int32(sb.physics))
		w.exp(sb.physicsExp)
		w.i32(int32(sb.playerNo))
		w.scb([]scParam(sb.stateDef))
		w.u32(uint32(len(sb.ctrlsps)))
		for _, x := range sb.ctrlsps {
			w.i32(x)
		}
		w.i32(sb.numVars)
		w.block(&sb.block)
	}
	w.b = append(w.b, "END!"...)

	name := strings.NewReplacer("/", "_", "\\", "_", "..", "").Replace(cdef)
	os.MkdirAll("cooked", 0o755)
	path := "cooked/" + name + ".cvfc"
	if err := os.WriteFile(path, w.b, 0o644); err != nil {
		vitaTrace("cook: cannot write " + path + ": " + err.Error())
		return
	}
	vitaTrace("cook: wrote " + path + " (" + itoa(len(w.b)) + " bytes, " + itoa(len(skeys)) + " states, " + itoa(len(pool)) + " strings)")
}
