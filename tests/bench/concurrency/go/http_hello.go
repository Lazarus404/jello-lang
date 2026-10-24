// 50 keep-alive clients each GET "hello world" 400 times from net/http over loopback.
package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"time"
)

const c, k = 50, 400

func main() {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	go http.Serve(listener, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "hello world")
	}))
	url := "http://" + listener.Addr().String() + "/"
	client := &http.Client{Transport: &http.Transport{MaxIdleConnsPerHost: c}}
	counts := make(chan int)
	start := time.Now()
	for i := 0; i < c; i++ {
		go func() {
			right := 0
			for j := 0; j < k; j++ {
				res, err := client.Get(url)
				if err != nil {
					panic(err)
				}
				body, err := io.ReadAll(res.Body)
				res.Body.Close()
				if err == nil && res.StatusCode == 200 && string(body) == "hello world" {
					right++
				}
			}
			counts <- right
		}()
	}
	total := 0
	for i := 0; i < c; i++ {
		total += <-counts
	}
	if total != c*k {
		panic("wrong count")
	}
	fmt.Printf("ms=%d\n", time.Since(start).Milliseconds())
}
