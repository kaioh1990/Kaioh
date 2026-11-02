//go:build !vita

package main

import "fmt"

func main() {
	fmt.Println("math hash:", mathHash())
	fmt.Printf("%q\n", catHashes())
}
