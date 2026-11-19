package main

import (
	"fmt"
	"math"
)

func main() {
	// Deterministic input set: special values + LCG-spread values.
	ins := []float64{0, math.Copysign(0, -1), 1, -1, 0.5, 2, 3, 10, -10, 0.1, 1e-300, 1e300, math.Inf(1), math.Inf(-1), math.NaN(), 1 << 29, 1e10, -1e10, 3.14159, 1e-8, 700, -700, 710, 0.66, 2.5, -0.7}
	s := uint64(88172645463325252)
	for i := 0; i < 4000; i++ {
		s ^= s << 13
		s ^= s >> 7
		s ^= s << 17
		e := int(s%40) - 20
		m := float64(s>>11) / (1 << 53)
		v := math.Ldexp(m, e)
		if s&1 == 1 {
			v = -v
		}
		ins = append(ins, v)
	}
	h := uint64(1469598103934665603)
	mix := func(f float64) { h = (h ^ math.Float64bits(f)) * 1099511628211 }
	for k, x := range ins {
		y := ins[(k*7+3)%len(ins)]
		mix(cvfExp(x))
		mix(cvfLog(math.Abs(x)))
		mix(cvfLog2(math.Abs(x)))
		mix(cvfPow(math.Abs(x), y))
		mix(cvfPow(x, 3))
		mix(cvfPow(x, -2))
		mix(cvfHypot(x, y))
		mix(cvfSin(x))
		mix(cvfCos(x))
		mix(cvfTan(x))
		mix(cvfAtan(x))
		mix(cvfAsin(x / 21))
		mix(cvfAcos(x / 21))
		mix(cvfAtan2(y, x))
	}
	fmt.Printf("0x%016xull\n", h)
}
