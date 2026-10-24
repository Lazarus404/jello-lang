// 1,000 goroutines in a ring pass a token for 1,000 laps.
package main

import (
	"fmt"
	"time"
)

const n, laps = 1000, 1000

func main() {
	start := time.Now()
	done := make(chan int)
	links := make([]chan int, n)
	for i := range links {
		links[i] = make(chan int, 1)
	}
	for i := 0; i < n; i++ {
		in, next := links[i], links[(i+1)%n]
		go func() {
			for k := 0; k < laps; k++ {
				hops := <-in
				if hops == 1 {
					done <- k
				} else {
					next <- hops - 1
				}
			}
		}()
	}
	links[0] <- n * laps
	if <-done != laps-1 {
		panic("wrong lap")
	}
	fmt.Printf("ms=%d\n", time.Since(start).Milliseconds())
}
