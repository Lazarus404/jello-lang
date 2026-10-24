// 50 connections each make 2,000 round trips of 64 bytes through an echo server over loopback.
package main

import (
	"bytes"
	"fmt"
	"io"
	"net"
	"time"
)

const c, k = 50, 2000

var message = []byte("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

func main() {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	go func() {
		for {
			s, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				buf := make([]byte, 65536)
				for {
					n, err := s.Read(buf)
					if n == 0 || err != nil {
						s.Close()
						return
					}
					s.Write(buf[:n])
				}
			}()
		}
	}()
	addr := listener.Addr().String()
	counts := make(chan int)
	start := time.Now()
	for i := 0; i < c; i++ {
		go func() {
			s, err := net.Dial("tcp", addr)
			if err != nil {
				panic(err)
			}
			got := make([]byte, 64)
			right := 0
			for j := 0; j < k; j++ {
				s.Write(message)
				if _, err := io.ReadFull(s, got); err != nil {
					panic(err)
				}
				if bytes.Equal(got, message) {
					right++
				}
			}
			s.Close()
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
