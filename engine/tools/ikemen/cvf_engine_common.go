package main

// Build configurations of the native C engine (tools/gen/gotoc):
//   - tag "cvf": desktop/mobile C engine (SDL window, GL renderer)
//   - tag "vita": PS Vita (vitaGL glue), as the Go Vita port
//
// vitaPerf selects the performance paths written for the Vita port; they
// are simulation-equivalent and used by every native build.
const vitaPerf = isVita || cvfEngine

// coopGoroutines: goroutines only switch at blocking operations and
// runtime.Gosched (TinyGo on the Vita, core/sched.c in the C engine), so
// polling loops must yield for background loaders to make progress.
const coopGoroutines = isVita || cvfEngine

func itoa(i int) string {
	neg := i < 0
	if neg {
		i = -i
	}
	b := []byte{}
	for {
		b = append([]byte{byte('0' + i%10)}, b...)
		i /= 10
		if i == 0 {
			break
		}
	}
	if neg {
		b = append([]byte{'-'}, b...)
	}
	return string(b)
}
