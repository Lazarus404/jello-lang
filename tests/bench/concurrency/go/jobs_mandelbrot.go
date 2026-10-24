// A 1,600 by 1,600 Mandelbrot set at 200 iterations a point, its rows spread
// over GOMAXPROCS goroutines claiming chunks of 8 rows.
package main

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
)

const size, iter = 1600, 200

func parallelFor(count, grain int, body func(first, end int)) {
	chunks := int32((count-1)/grain + 1)
	var next atomic.Int32
	var wg sync.WaitGroup
	for w := 0; w < runtime.GOMAXPROCS(0) && w < int(chunks); w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for c := next.Add(1) - 1; c < chunks; c = next.Add(1) - 1 {
				first := int(c) * grain
				body(first, min(first+grain, count))
			}
		}()
	}
	wg.Wait()
}

func main() {
	inside := make([]uint8, size*size)
	start := time.Now()
	parallelFor(size, 8, func(first, end int) {
		for y := first; y < end; y++ {
			ci := 2.0*float64(y)/float64(size) - 1.0
			for x := 0; x < size; x++ {
				cr := 2.0*float64(x)/float64(size) - 1.5
				zr, zi, k := 0.0, 0.0, 0
				for k < iter && zr*zr+zi*zi <= 4.0 {
					zr, zi = zr*zr-zi*zi+cr, 2.0*zr*zi+ci
					k++
				}
				if k == iter {
					inside[y*size+x] = 1
				}
			}
		}
	})
	ms := time.Since(start).Milliseconds()
	count := 0
	for _, v := range inside {
		count += int(v)
	}
	if count != 976330 {
		panic(fmt.Sprintf("count %d", count))
	}
	fmt.Printf("ms=%d\n", ms)
}
