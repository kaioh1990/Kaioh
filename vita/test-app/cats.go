package main

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math"
)

var catNames = []string{"exp", "log", "pow", "hypot", "log2", "sin", "cos", "tan", "atan2", "asin", "acos", "atan", "sqrt", "f32arith", "floor", "denorm64", "denorm32", "mod", "ldexp", "f32div", "f32conv"}

// catHashes hashes each math category separately so a mismatch can be pinned down.
func catHashes() []string {
	out := make([]string, len(catNames))
	for k := range catNames {
		h := sha256.New()
		put := func(f float64) {
			var b [8]byte
			binary.LittleEndian.PutUint64(b[:], math.Float64bits(f))
			h.Write(b[:])
		}
		put32 := func(f float32) {
			var b [4]byte
			binary.LittleEndian.PutUint32(b[:], math.Float32bits(f))
			h.Write(b[:])
		}
		seed := uint64(88172645463325252)
		rnd := func() float64 {
			seed ^= seed << 13
			seed ^= seed >> 7
			seed ^= seed << 17
			return float64(seed%2000000)/1000.0 - 1000.0
		}
		for i := 0; i < 20000; i++ {
			x, y := rnd(), rnd()/100
			ax := math.Abs(x) + 1e-3
			switch k {
			case 0:
				put(cvfExp(y))
			case 1:
				put(cvfLog(ax))
			case 2:
				put(cvfPow(ax/100, y))
			case 3:
				put(cvfHypot(x, y))
			case 4:
				put(cvfLog2(ax))
			case 5:
				put(cvfSin(x))
			case 6:
				put(cvfCos(y))
			case 7:
				put(cvfTan(y))
			case 8:
				put(cvfAtan2(x, y))
			case 9:
				put(cvfAsin(y / 10))
			case 10:
				put(cvfAcos(y / 10))
			case 11:
				put(cvfAtan(x))
			case 12:
				put(math.Sqrt(ax))
			case 13:
				fx, fy := float32(x), float32(y)
				put32(fx*fy + fx/(fy+1.5))
			case 14:
				put(math.Floor(x * 1.37))
			case 15:
				put(1e-310 * (1 + float64(i%7)) * 0.5)
			case 16:
				put32(float32(1e-39) * float32(1+i%5))
			case 17:
				put(math.Mod(x, 7.3))
			case 18:
				put(math.Ldexp(x, i%40-20))
			case 19:
				put32(float32(x) / float32(y+0.37))
			case 20:
				put32(float32(x * 1.0000001))
			}
		}
		out[k] = fmt.Sprintf("%x", h.Sum(nil)[:4])
	}
	return out
}
