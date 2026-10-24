// 16 sockets each make 5,000 round trips of a 64-byte datagram through a UDP echo server.
package main

import (
	"bytes"
	"fmt"
	"net"
	"time"
)

const c, k = 16, 5000

var message = []byte("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

func main() {
	server, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		panic(err)
	}
	go func() {
		buf := make([]byte, 65536)
		for {
			n, from, err := server.ReadFromUDP(buf)
			if err != nil {
				return
			}
			server.WriteToUDP(buf[:n], from)
		}
	}()
	to := server.LocalAddr().(*net.UDPAddr)
	counts := make(chan int)
	start := time.Now()
	for i := 0; i < c; i++ {
		go func() {
			s, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
			if err != nil {
				panic(err)
			}
			buf := make([]byte, 65536)
			right := 0
			for j := 0; j < k; j++ {
				s.WriteToUDP(message, to)
				n, _, err := s.ReadFromUDP(buf)
				if err != nil {
					panic(err)
				}
				if bytes.Equal(buf[:n], message) {
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
