// IDLE_N goroutines (default 100,000) all blocked receiving at once; the
// harness reads the peak resident memory.
package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func main() {
	n := 100000
	if s := os.Getenv("IDLE_N"); s != "" {
		n, _ = strconv.Atoi(s)
	}
	start := time.Now()
	ready := make(chan int)
	var wg sync.WaitGroup
	parked := make([]chan int, n)
	for i := range parked {
		in := make(chan int)
		parked[i] = in
		wg.Add(1)
		go func() { ready <- 1; <-in; wg.Done() }()
	}
	for i := 0; i < n; i++ {
		<-ready
	}
	for _, in := range parked {
		in <- 0
	}
	wg.Wait()
	fmt.Printf("ms=%d\n", time.Since(start).Milliseconds())
}
