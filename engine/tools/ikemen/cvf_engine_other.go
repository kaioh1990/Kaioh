//go:build !cvf

package main

const cvfEngine = false

func cvfSelectRendererGLES(cfgVal string) (Renderer, FontRenderer, bool) { return nil, nil, false }
