//go:build !vita

package main

import "fmt"

func main() {
	fmt.Println("math hash:", mathHash())
	fmt.Println(goroutineTest())
	fmt.Printf("bench: %.0f steps/s\n", cpuBench())
}
