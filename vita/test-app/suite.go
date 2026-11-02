package main

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math"
	"sort"
	"strings"
	"sync"
	"time"
)

// mathHash runs the engine's deterministic math (and plain float32/float64
// arithmetic, including denormals) over fixed inputs. A PC and a Vita must
// print the same hash for rollback cross-play to stay in sync.
func mathHash() string {
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
	for i := 0; i < 200000; i++ {
		x, y := rnd(), rnd()/100
		ax := math.Abs(x) + 1e-3
		put(cvfExp(y))
		put(cvfLog(ax))
		put(cvfPow(ax/100, y))
		put(cvfHypot(x, y))
		put(cvfLog2(ax))
		put(cvfSin(x) * cvfCos(y))
		put(math.Sqrt(ax) / 3)
		// float32 paths (the engine simulates mostly in float32)
		fx, fy := float32(x), float32(y)
		put32(fx*fy + fx/(fy+1.5))
		put32(float32(math.Floor(float64(fx * 1.37))))
		// denormals: a CPU that flushes them to zero gives a different hash
		d := 1e-310 * (1 + float64(i%7))
		put(d * 0.5)
		put32(float32(1e-39) * float32(1+i%5))
	}
	return fmt.Sprintf("%x", h.Sum(nil))[:16]
}

func goroutineTest() string {
	ch := make(chan int)
	done := make(chan bool)
	sum := 0
	go func() {
		for v := range ch {
			sum += v
		}
		done <- true
	}()
	var wg sync.WaitGroup
	for w := 0; w < 4; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			for i := 0; i < 1000; i++ {
				ch <- 1
			}
		}(w)
	}
	time.Sleep(20 * time.Millisecond)
	wg.Wait()
	close(ch)
	<-done
	m := map[string]int{}
	for i := 0; i < 1000; i++ {
		m[fmt.Sprint(i%37)]++
	}
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return fmt.Sprintf("goroutines sum=%d (want 4000), map keys=%d (want 37) %s", sum, len(keys), strings.Join(keys[:3], ","))
}

// cpuBench returns how many simulated "engine-like" float32 steps per second run.
func cpuBench() float64 {
	type obj struct{ x, y, vx, vy float32 }
	objs := make([]obj, 256)
	for i := range objs {
		objs[i] = obj{float32(i), float32(i * 2), 1.5, -0.75}
	}
	start := time.Now()
	steps := 0
	for time.Since(start) < 2*time.Second {
		for i := range objs {
			o := &objs[i]
			o.vy += 0.44
			o.x += o.vx
			o.y += o.vy
			if o.y > 200 {
				o.y = 200
				o.vy = -o.vy * 0.5
			}
		}
		steps++
	}
	return float64(steps) / time.Since(start).Seconds()
}
