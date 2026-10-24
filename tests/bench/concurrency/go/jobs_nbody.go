// 800 bodies under gravity for 40 steps: each step's accelerations, then its
// moves, spread over GOMAXPROCS goroutines claiming chunks.
package main

import (
	"fmt"
	"math"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
)

const bodies, steps, dt = 800, 40, 0.001

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
	position := make([]float64, 3*bodies)
	velocity := make([]float64, 3*bodies)
	for i := 0; i < bodies; i++ {
		position[3*i] = float64((i*37)%101) / 10.0
		position[3*i+1] = float64((i*59)%103) / 10.0
		position[3*i+2] = float64((i*71)%107) / 10.0
	}
	start := time.Now()
	for s := 0; s < steps; s++ {
		parallelFor(bodies, 16, func(first, end int) {
			for a := first; a < end; a++ {
				x, y, z := position[3*a], position[3*a+1], position[3*a+2]
				ax, ay, az := 0.0, 0.0, 0.0
				for b := 0; b < bodies; b++ {
					dx, dy, dz := position[3*b]-x, position[3*b+1]-y, position[3*b+2]-z
					d2 := dx*dx + dy*dy + dz*dz + 0.01
					f := 1.0 / (d2 * math.Sqrt(d2))
					ax, ay, az = ax+dx*f, ay+dy*f, az+dz*f
				}
				velocity[3*a] += dt * ax
				velocity[3*a+1] += dt * ay
				velocity[3*a+2] += dt * az
			}
		})
		parallelFor(3*bodies, 96, func(first, end int) {
			for k := first; k < end; k++ {
				position[k] += dt * velocity[k]
			}
		})
	}
	ms := time.Since(start).Milliseconds()
	sum := 0.0
	for _, v := range velocity {
		sum += v * v
	}
	if math.Abs(sum-253.27778330888) > 1e-7 {
		panic(fmt.Sprintf("sum %.12f", sum))
	}
	fmt.Printf("ms=%d\n", ms)
}
