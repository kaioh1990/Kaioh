package main

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math"
)

func ulp(a, b float64) int64 {
	if a == b || (math.IsNaN(a) && math.IsNaN(b)) {
		return 0
	}
	d := int64(math.Float64bits(a)) - int64(math.Float64bits(b))
	if d < 0 {
		d = -d
	}
	return d
}

func main() {
	// Hashes 10M results of the deterministic math functions; every CPU must print the same hash.
	h := sha256.New()
	var maxU [5]int64
	seed := uint64(88172645463325252)
	rnd := func() float64 { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return float64(seed%2000000)/1000.0 - 1000.0 }
	put := func(f float64) { var b [8]byte; binary.LittleEndian.PutUint64(b[:], math.Float64bits(f)); h.Write(b[:]) }
	for i := 0; i < 2000000; i++ {
		x, y := rnd(), rnd()/100
		e := cvfExp(y); put(e); if u := ulp(e, math.Exp(y)); u > maxU[0] { maxU[0] = u }
		ax := math.Abs(x) + 1e-3
		l := cvfLog(ax); put(l); if u := ulp(l, math.Log(ax)); u > maxU[1] { maxU[1] = u }
		p := cvfPow(ax/100, y); put(p); if u := ulp(p, math.Pow(ax/100, y)); u > maxU[2] { maxU[2] = u }
		hy := cvfHypot(x, y); put(hy); if u := ulp(hy, math.Hypot(x, y)); u > maxU[3] { maxU[3] = u }
		l2 := cvfLog2(ax); put(l2); if u := ulp(l2, math.Log2(ax)); u > maxU[4] { maxU[4] = u }
	}
	fmt.Printf("maxULP exp=%d log=%d pow=%d hypot=%d log2=%d\nhash=%x\n", maxU[0], maxU[1], maxU[2], maxU[3], maxU[4], h.Sum(nil))
}
