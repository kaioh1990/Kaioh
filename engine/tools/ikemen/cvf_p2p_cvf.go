//go:build cvf

package main

// Nearby direct link in the native C engine. On iPhone/iPad the app's
// CvfP2P.m (Network.framework peer-to-peer) registers a function table
// (CvfP2PRegister); the functions below are implemented in C
// (src/ik/ik_native.c, GOTOC_NATIVE_FUNCS) and call through that table, as
// cvf_p2p_ios.go does in the Go app. Elsewhere the table stays empty and
// they do nothing.

import "runtime"

var cvfP2PAvailable = runtime.GOOS == "ios"

func cvfP2PAdvertise(id, name, ver string, tcpPort, udpPort int) {}
func cvfP2PAdvertiseStop()                                       {}
func cvfP2PBrowse(on bool)                                       {}
func cvfP2PJoin(id string, localTcp, localUdp int) bool          { return false }
func cvfP2PClose()                                               {}

// Opens the app's name box. False if the app has none.
func cvfNativeAskName(suggestion string) bool { return false }
