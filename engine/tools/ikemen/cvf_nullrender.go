package main

import mgl "github.com/go-gl/mathgl/mgl32"

// nullRenderer: the renderer of the headless native engine (reference
// runs, servers). Every GPU operation is a no-op; textures only remember
// their size, so loading code behaves as with a real renderer.
type nullRenderer struct{}

type nullTexture struct{ w, h int32 }

func (t *nullTexture) SetData(data []byte)                                         {}
func (t *nullTexture) SetSubData(data []byte)                                      {}
func (t *nullTexture) SetSubData2(data []byte, x, y, width, height, stride int32) {}
func (t *nullTexture) SetDataG(data []byte, mag, min, ws, wt TextureSamplingParam)  {}
func (t *nullTexture) SetPixelData(data []float32)                                 {}
func (t *nullTexture) IsValid() bool                                               { return true }
func (t *nullTexture) GetWidth() int32                                             { return t.w }
func (t *nullTexture) GetHeight() int32                                            { return t.h }
func (t *nullTexture) GetUV() [4]float32                                           { return [4]float32{0, 0, 1, 1} }
func (t *nullTexture) CopyData(src *Texture)                                       {}
func (t *nullTexture) MarkNonSwappable()                                           {}

func (r *nullRenderer) GetName() string                                                 { return "null" }
func (r *nullRenderer) DebugInfo() string                                               { return "" }
func (r *nullRenderer) Init()                                                           {}
func (r *nullRenderer) Close()                                                          {}
func (r *nullRenderer) BeginFrame(clearColor bool)                                      {}
func (r *nullRenderer) EndFrame()                                                       {}
func (r *nullRenderer) Await()                                                          {}
func (r *nullRenderer) IsModelEnabled() bool                                            { return false }
func (r *nullRenderer) IsShadowEnabled() bool                                           { return false }
func (r *nullRenderer) LoadCustomSpriteShader(shaderName string, shaderData []byte) uint32 { return 0 }
func (r *nullRenderer) UnloadCustomSpriteShader(shaderName string)                      {}
func (r *nullRenderer) SetSpritePipeline(shaderName string)                             {}
func (r *nullRenderer) SetCustomUniforms(params [16]float32)                            {}
func (r *nullRenderer) NeedsGrabPass() bool                                             { return false }
func (r *nullRenderer) ResolveBackBuffer() Texture                                      { return &nullTexture{} }
func (r *nullRenderer) EnableBlending(eq BlendEquation, src, dst BlendFunc)             {}
func (r *nullRenderer) DisableBlending()                                                {}
func (r *nullRenderer) prepareShadowMapPipeline(bufferIndex uint32)                     {}
func (r *nullRenderer) setShadowMapPipeline(doubleSided, invertFrontFace, useUV, useNormal, useTangent, useVertColor, useJoint0, useJoint1 bool, numVertices, vertAttrOffset uint32) {
}
func (r *nullRenderer) ReleaseShadowPipeline()                                  {}
func (r *nullRenderer) prepareModelPipeline(bufferIndex uint32, env *Environment) {}
func (r *nullRenderer) SetModelPipeline(eq BlendEquation, src, dst BlendFunc, depthTest, depthMask, doubleSided, invertFrontFace, useUV, useNormal, useTangent, useVertColor, useJoint0, useJoint1, useOutlineAttribute bool, numVertices, vertAttrOffset uint32) {
}
func (r *nullRenderer) SetMeshOutlinePipeline(invertFrontFace bool, meshOutline float32) {}
func (r *nullRenderer) ReleaseModelPipeline()                                          {}
func (r *nullRenderer) newTexture(width, height, depth int32, filter bool) (Texture, error) {
	return &nullTexture{width, height}, nil
}
func (r *nullRenderer) newPaletteTexture(slot [2]uint32, offset [2]int32, uvst [4]float32) Texture {
	return &nullTexture{256, 1}
}
func (r *nullRenderer) newSubTexture(tex Texture, width, height int32, uvst [4]float32) Texture {
	return &nullTexture{width, height}
}
func (r *nullRenderer) newModelTexture(width, height, depth int32, filter bool) (Texture, error) {
	return &nullTexture{width, height}, nil
}
func (r *nullRenderer) newDataTexture(width, height int32) (Texture, error) {
	return &nullTexture{width, height}, nil
}
func (r *nullRenderer) newHDRTexture(width, height int32) (Texture, error) {
	return &nullTexture{width, height}, nil
}
func (r *nullRenderer) newCubeMapTexture(widthHeight int32, mipmap bool, lowestMipLevel int32) (Texture, error) {
	return &nullTexture{widthHeight, widthHeight}, nil
}
func (r *nullRenderer) ReadPixels(data []uint8, width, height int)                  {}
func (r *nullRenderer) BeginScreenshot(width, height int)                           {}
func (r *nullRenderer) FinishScreenshot(data []uint8, width, height int) bool       { return false }
func (r *nullRenderer) EnableScissor(x, y, width, height int32)                     {}
func (r *nullRenderer) DisableScissor()                                             {}
func (r *nullRenderer) SetUniformI(name string, val int)                            {}
func (r *nullRenderer) SetUniformF(name string, values ...float32)                  {}
func (r *nullRenderer) SetUniformFv(name string, values []float32)                  {}
func (r *nullRenderer) SetUniformMatrix(name string, value []float32)               {}
func (r *nullRenderer) SetTexture(name string, tex Texture)                         {}
func (r *nullRenderer) SetModelUniformI(name string, val int)                       {}
func (r *nullRenderer) SetModelUniformF(name string, values ...float32)             {}
func (r *nullRenderer) SetModelUniformFv(name string, values []float32)             {}
func (r *nullRenderer) SetModelUniformMatrix(name string, value []float32)          {}
func (r *nullRenderer) SetModelUniformMatrix3(name string, value []float32)         {}
func (r *nullRenderer) SetModelTexture(name string, t Texture)                      {}
func (r *nullRenderer) SetShadowMapUniformI(name string, val int)                   {}
func (r *nullRenderer) SetShadowMapUniformF(name string, values ...float32)         {}
func (r *nullRenderer) SetShadowMapUniformFv(name string, values []float32)         {}
func (r *nullRenderer) SetShadowMapUniformMatrix(name string, value []float32)      {}
func (r *nullRenderer) SetShadowMapUniformMatrix3(name string, value []float32)     {}
func (r *nullRenderer) SetShadowMapTexture(name string, t Texture)                  {}
func (r *nullRenderer) SetShadowFrameTexture(i uint32)                              {}
func (r *nullRenderer) SetShadowFrameCubeTexture(i uint32)                          {}
func (r *nullRenderer) SetVertexData(values ...float32)                             {}
func (r *nullRenderer) SetQuadVertices(v *[16]float32)                              {}
func (r *nullRenderer) SetModelVertexData(bufferIndex uint32, values []byte)        {}
func (r *nullRenderer) SetModelIndexData(bufferIndex uint32, values ...uint32)      {}
func (r *nullRenderer) RenderQuad()                                                 {}
func (r *nullRenderer) RenderQuadBatch(vertexCount int32)                           {}
func (r *nullRenderer) RenderElements(mode PrimitiveMode, count, offset int)        {}
func (r *nullRenderer) RenderShadowMapElements(mode PrimitiveMode, count, offset int) {}
func (r *nullRenderer) RenderCubeMap(envTexture Texture, cubeTexture Texture)       {}
func (r *nullRenderer) RenderFilteredCubeMap(distribution int32, cubeTexture Texture, filteredTexture Texture, mipmapLevel, sampleCount int32, roughness float32) {
}
func (r *nullRenderer) RenderLUT(distribution int32, cubeTexture Texture, lutTexture Texture, sampleCount int32) {}
func (r *nullRenderer) PerspectiveProjectionMatrix(angle, aspect, near, far float32) mgl.Mat4 {
	return mgl.Perspective(angle, aspect, near, far)
}
func (r *nullRenderer) OrthographicProjectionMatrix(left, right, bottom, top, near, far float32) mgl.Mat4 {
	return mgl.Ortho(left, right, bottom, top, near, far)
}
func (r *nullRenderer) SetVSync(interval int)  {}
func (r *nullRenderer) NewWorkerThread() bool { return false }

var _ Renderer = (*nullRenderer)(nil)
