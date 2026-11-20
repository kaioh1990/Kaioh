// Go side of the Nearby interop check (cvf_nearby_test): the probe/reply
// protocol exactly as cvf_nearby.go speaks it, with encoding/json.
//
//	go run . host     answers probes on UDP 7520 as "GoHost", port 7600, for 10 s
//	go run . probe    probes 127.0.0.1:7520 and checks the reply of a native host
package main

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"time"
)

const probe = "CVFNEARBY?1"

type reply struct {
	T       string `json:"t"`
	Name    string `json:"name"`
	Port    int    `json:"port"`
	Version string `json:"ver"`
	ID      string `json:"id"`
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "host" {
		conn, err := net.ListenPacket("udp4", ":7520")
		if err != nil {
			fmt.Println("FAIL listen:", err)
			os.Exit(1)
		}
		conn.SetReadDeadline(time.Now().Add(10 * time.Second))
		buf := make([]byte, 256)
		n := 0
		for {
			k, from, err := conn.ReadFrom(buf)
			if err != nil {
				break
			}
			if string(buf[:k]) == probe {
				b, _ := json.Marshal(reply{T: "cvfnearby", Name: "GoHost", Port: 7600, Version: "nearby-test-1", ID: "0123456789"})
				conn.WriteTo(b, from)
				n++
			}
		}
		fmt.Printf("go host answered %d probe(s)\n", n)
		return
	}
	conn, err := net.ListenPacket("udp4", ":0")
	if err != nil {
		fmt.Println("FAIL listen:", err)
		os.Exit(1)
	}
	to, _ := net.ResolveUDPAddr("udp4", "127.0.0.1:7520")
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	buf := make([]byte, 512)
	for i := 0; i < 50; i++ {
		conn.WriteTo([]byte(probe), to)
		conn.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
		k, _, err := conn.ReadFrom(buf)
		if err != nil {
			continue
		}
		var r reply
		if err := json.Unmarshal(buf[:k], &r); err != nil {
			fmt.Println("FAIL json:", err, string(buf[:k]))
			os.Exit(1)
		}
		if r.T != "cvfnearby" || r.Name != "Tester" || r.Port != 7555 || r.Version != "nearby-test-1" || len(r.ID) != 10 {
			fmt.Printf("FAIL reply %+v\n", r)
			os.Exit(1)
		}
		fmt.Printf("ok   go probe: native reply %s\n", buf[:k])
		return
	}
	fmt.Println("FAIL no reply from the native host")
	os.Exit(1)
}
