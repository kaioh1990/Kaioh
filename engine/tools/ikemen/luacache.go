package main

import (
	lua "github.com/yuin/gopher-lua"
)

// luaChunkCache holds compiled versions of Lua snippets the engine runs every
// frame (Common.Lua "loop()", motif hooks). DoString re-parses and recompiles
// the source each call, which on the Vita cost ~10 parses per frame.
var luaChunkCache = map[*lua.LState]map[string]*lua.LFunction{}

func luaDoStringCached(l *lua.LState, src string) error {
	if l == nil { // headless runner: no Lua state (scripts drive menus only)
		return nil
	}
	m := luaChunkCache[l]
	if m == nil {
		m = map[string]*lua.LFunction{}
		luaChunkCache[l] = m
	}
	fn, ok := m[src]
	if !ok {
		var err error
		if fn, err = l.LoadString(src); err != nil {
			return err
		}
		m[src] = fn
	}
	l.Push(fn)
	return l.PCall(0, lua.MultRet, nil)
}
