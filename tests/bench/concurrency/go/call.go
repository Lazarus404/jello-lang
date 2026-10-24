// 200,000 request/reply round trips between two goroutines.
package main

import (
	"fmt"
	"time"
)

const n = 200000

type request struct {
	n     int
	reply chan int
}

func main() {
	server := make(chan request)
	go func() {
		for r := range server {
			r.reply <- r.n + 1
		}
	}()
	start := time.Now()
	right := 0
	for i := 0; i < n; i++ {
		reply := make(chan int, 1)
		server <- request{i, reply}
		if <-reply == i+1 {
			right++
		}
	}
	if right != n {
		panic("wrong reply")
	}
	fmt.Printf("ms=%d\n", time.Since(start).Milliseconds())
}
