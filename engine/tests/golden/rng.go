package main

import (
	"fmt"
	"math"
)

const IMax = int32(^uint32(0) >> 1)

var seed int32

func Btoi(b bool) int32 {
	if b {
		return 1
	}
	return 0
}
func Random() int32 {
	w := seed / 127773
	seed = (seed-w*127773)*16807 - w*2836
	if seed <= 0 {
		seed += IMax - Btoi(seed == 0)
	}
	return seed
}
func Rand(min, max int32) int32 { return min + Random()/(IMax/(max-min+1)+1) }
func RandF(x, y float32) float32 { return x + float32(Random())*(y-x)/float32(IMax) }
func RandF32(min, max float32) float32 {
	return min + float32(Random())/(float32(IMax)/(max-min+1.0)+1.0)
}

func main() {
	seed = 12345
	h := uint32(2166136261)
	mix := func(v uint32) { h = (h ^ v) * 16777619 }
	for i := 0; i < 100000; i++ {
		switch i % 4 {
		case 0:
			mix(uint32(Random()))
		case 1:
			mix(uint32(Rand(0, 999)))
		case 2:
			mix(math.Float32bits(RandF(-3.5, 7.25)))
		case 3:
			mix(math.Float32bits(RandF32(1, 6)))
		}
	}
	fmt.Printf("0x%08x %d\n", h, seed)
}
