package main

import (
	"fmt"
	"math"
	"strings"
)

const IMax = int32(math.MaxInt32)
const IErr = ^IMax

func Atoi(str string) int32 {
	var n int64
	str = strings.TrimSpace(str)
	if len(str) > 0 {
		var a string
		if str[0] == '-' || str[0] == '+' {
			a = str[1:]
		} else {
			a = str
		}
		limit := int64(math.MaxInt32)
		if str[0] == '-' {
			limit++
		}
		for i := range a {
			if a[i] < '0' || '9' < a[i] {
				break
			}
			n = n*10 + int64(a[i]-'0')
			if n > limit {
				if str[0] == '-' {
					return IErr
				}
				return IMax
			}
		}
		if str[0] == '-' {
			n *= -1
		}
	}
	return int32(n)
}

func Atof(str string) float64 {
	f := 0.0
	str = strings.TrimSpace(str)
	if len(str) > 0 {
		var a string
		if str[0] == '-' || str[0] == '+' {
			a = str[1:]
		} else {
			a = str
		}
		i, p := 0, 0
		for ; i < len(a); i++ {
			if a[i] == '.' {
				if p != 0 {
					break
				}
				p = i + 1
				continue
			}
			if a[i] < '0' || '9' < a[i] {
				break
			}
			f = f*10 + float64(a[i]-'0')
		}
		e := 0.0
		if i+1 < len(a) && (a[i] == 'e' || a[i] == 'E') {
			j := i + 1
			if a[j] == '-' || a[j] == '+' {
				j++
			}
			for ; j < len(a) && '0' <= a[j] && a[j] <= '9'; j++ {
				e = e*10 + float64(a[j]-'0')
			}
			if e != 0 {
				if str[i+1] == '-' {
					e *= -1
				}
				if p == 0 {
					p = i
				}
			}
		}
		if p > 0 {
			f *= math.Pow10(p - i + int(e))
		}
		if str[0] == '-' {
			f *= -1
		}
	}
	return f
}

var cases = []string{"0", "1", "-1", " 42 ", "+7", "3.14159", "-0.5", ".25", "1e3", "1.5e-2", "-2.5E+3", "12abc", "1.2.3", "2147483647", "2147483648", "-2147483648", "-2147483649", "0.1", "0.3333333333", "123456.789", "1e-5", "-1e-5", "9.81", "", "  ", "-", "0.01", "65536.5", "1.0e400", "5e", "-.75", "100.000001"}

func main() {
	fmt.Println("static const struct { const char *s; i32 i; u64 f; } atof_cases[] = {")
	for _, c := range cases {
		fmt.Printf("    {%q, %d, 0x%016xull},\n", c, Atoi(c), math.Float64bits(Atof(c)))
	}
	fmt.Println("};")
}
