//go:build cvfgohead

package main

import (
	"fmt"
	"os"
	"sync"

	"github.com/ikemen-engine/beep/v2"
)

// Go build of the headless runner (tests/netplay, reference statedumps):
//
//	go build -tags cvf,cvfgohead -o cvf_headless_go ./src
//	cvf_headless_go <gamedir> [ikemen args...]
//
// The same entry point as the native engine's cvf_headless
// (src/app/headless_main.c), run from an init function so that main()
// (window, renderer, Lua) never starts.
func init() {
	speaker = &cvfNullSpeaker{} // no audio device: mixing state only, as the native headless build
	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr, "usage: %s <gamedir> [ikemen args...]\n", os.Args[0])
		os.Exit(2)
	}
	if err := os.Chdir(os.Args[1]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	w := cvfHeadlessMain(os.Args[2:])
	fmt.Printf("winner: %d\n", w)
	if w < 0 {
		os.Exit(1)
	}
	os.Exit(0)
}

// AudioSink without a device (System.init, which opens the SDL one, does
// not run headless).
type cvfNullSpeaker struct{ mu sync.Mutex }

func (s *cvfNullSpeaker) Init(sr beep.SampleRate, bufferSize int) error { return nil }
func (s *cvfNullSpeaker) Play(st beep.Streamer)                         {}
func (s *cvfNullSpeaker) Lock()                                         { s.mu.Lock() }
func (s *cvfNullSpeaker) Unlock()                                       { s.mu.Unlock() }
func (s *cvfNullSpeaker) Close()                                        {}
func (s *cvfNullSpeaker) FillAudio()                                    {}
