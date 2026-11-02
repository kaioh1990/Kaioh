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
unsigned vfp_get_fpscr(void);
void vfp_set_ieee(void);
int mem_probe_mb(void);
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
	say("\x1b[1;33mCastlevania Fighter Online - Vita test 9\x1b[0m\n\n")

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

	say("2. Rollback math check (must match PC/Mac/iPad)\n")
	want := []string{"b42e237c", "3a2e4fbc", "b00ff867", "3219fa5d", "553bdcab", "c4e876c5", "00d81096", "a0dac36e", "13204710", "926a095a", "4e435bb3", "ecdf26ee", "9e0e55e2", "dcba6ce0", "9bf1c1b7", "3404666c", "5a500614", "741dd2fb", "2336ec2b", "27d7d8cd", "8cee5ea0"}
	check := func(label string) {
		say("   %s FPSCR=0x%08x: ", label, uint32(C.vfp_get_fpscr()))
		got := catHashes()
		bad := []string{}
		for i := range got {
			if got[i] != want[i] {
				bad = append(bad, catNames[i])
			}
		}
		if len(bad) == 0 {
			say("\x1b[1;32mALL MATCH\x1b[0m\n")
		} else {
			say("\x1b[1;31mdiffer: %v\x1b[0m\n", bad)
		}
	}
	check("default")
	C.vfp_set_ieee()
	check("IEEE   ")
	h := mathHash()
	say("   full hash %s (PC %s)\n", h, expectedHash)

	say("3. CPU speed: ")
	b := cpuBench()
	say("%.0f engine-like steps/s (PC reference ~2.8M)\n", b)

	say("4. Memory: ")
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	say("Go heap %d MB; largest free block now %d MB\n", ms.Sys>>20, int(C.mem_probe_mb()))
	say("\n\x1b[1;36mAll done. Press START to exit.\x1b[0m\n")
	for C.startPressed() == 0 {
		time.Sleep(50 * time.Millisecond)
	}
	C.sceKernelExitProcess(0)
}
