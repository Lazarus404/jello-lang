// Starts 1,000,000 goroutines, then waits for them all.
package main

import (
	"fmt"
	"sync"
	"time"
)

const n = 1000000

func main() {
	start := time.Now()
	var wg sync.WaitGroup
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func() { wg.Done() }()
	}
	wg.Wait()
	fmt.Printf("ms=%d\n", time.Since(start).Milliseconds())
}
