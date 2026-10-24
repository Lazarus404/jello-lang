// One sender sends 100 messages to each of 10,000 receiving goroutines.
package main

import (
	"fmt"
	"time"
)

const r, m = 10000, 100

func main() {
	start := time.Now()
	sums := make(chan int)
	receivers := make([]chan int, r)
	for i := range receivers {
		in := make(chan int, m)
		receivers[i] = in
		go func() {
			sum := 0
			for k := 0; k < m; k++ {
				sum += <-in
			}
			sums <- sum
		}()
	}
	for k := 0; k < m; k++ {
		for _, in := range receivers {
			in <- k
		}
	}
	total := 0
	for i := 0; i < r; i++ {
		total += <-sums
	}
	if total != r*(m*(m-1)/2) {
		panic("wrong total")
	}
	fmt.Printf("ms=%d\n", time.Since(start).Milliseconds())
}
