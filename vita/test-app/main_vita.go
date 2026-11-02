//go:build vita

package main

/*
#cgo CFLAGS: -std=c2x
#cgo LDFLAGS: -lSceDisplay_stub -lSceCtrl_stub -lSceIofilemgr_stub
#include <stdint.h>
#include "debugScreen.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <stdlib.h>

void vlog(const char *s);
static void say(const char *s) { psvDebugScreenPuts(s); vlog(s); }
static int startPressed(void) {
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);
	return (pad.buttons & SCE_CTRL_START) != 0;
}
*/
import "C"

import (
	"fmt"
	"runtime"
	"time"
	"unsafe"
)

const expectedHash = "e7ccaee0656fb0ad"

func say(format string, a ...interface{}) {
	s := C.CString(fmt.Sprintf(format, a...))
	C.say(s)
	C.free(unsafe.Pointer(s))
}

func vlog(m string) {
	s := C.CString(m)
	C.vlog(s)
	C.free(unsafe.Pointer(s))
}

func main() {
	vlog("1 Go main started")
	C.psvDebugScreenInit()
	vlog("2 screen ready")
	say("\x1b[1;33mCastlevania Fighter Online - Vita test 8\x1b[0m\n\n")

	say("1. Goroutines:\n")
	say("   a) start one... ")
	done := make(chan bool, 1)
	go func() { done <- true }()
	say("created... ")
	<-done
	say("ok\n   b) ping-pong... ")
	ping, pong := make(chan int), make(chan int)
	go func() {
		for v := range ping {
			pong <- v + 1
		}
	}()
	n := 0
	for i := 0; i < 100; i++ {
		ping <- n
		n = <-pong
	}
	close(ping)
	say("%d (want 100)\n   c) sleep... ", n)
	time.Sleep(100 * time.Millisecond)
	say("ok\n   d) full test: ")
	say("%s\n", goroutineTest())

	say("2. Rollback math check (must match PC/Mac/iPad)...\n")
	t := time.Now()
	h := mathHash()
	ok := "\x1b[1;32mMATCH - Vita can play online vs PC\x1b[0m"
	if h != expectedHash {
		ok = "\x1b[1;31mMISMATCH - would desync vs PC\x1b[0m"
	}
	say("   hash %s (PC: %s) %s  [%.1fs]\n", h, expectedHash, ok, time.Since(t).Seconds())

	say("3. CPU speed: ")
	b := cpuBench()
	say("%.0f engine-like steps/s (PC reference ~2.8M)\n", b)

	say("4. Memory: ")
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	say("Go heap reserved %d MB\n", ms.Sys>>20)
	// How much can Go actually use?
	var keep [][]byte
	total := 0
	for total < 512 {
		func() {
			defer func() { recover() }()
			keep = append(keep, make([]byte, 8<<20))
			total += 8
		}()
		if len(keep)*8 != total {
			break
		}
		if total%64 == 0 {
			say("%d.. ", total)
		}
	}
	say("\n   allocated %d MB before running out\n", total)
	keep = nil

	say("\n\x1b[1;36mAll done. Press START to exit.\x1b[0m\n")
	for C.startPressed() == 0 {
		time.Sleep(50 * time.Millisecond)
	}
	C.sceKernelExitProcess(0)
}
