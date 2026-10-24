# 16 sockets each make 5,000 round trips of a 64-byte datagram through a UDP echo server.
defmodule EchoUdp do
  @c 16
  @k 5000
  @message "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  @opts [:binary, active: false, ip: {127, 0, 0, 1}]

  def serve(s) do
    {:ok, {host, port, data}} = :gen_udp.recv(s, 0)
    :ok = :gen_udp.send(s, host, port, data)
    serve(s)
  end

  def client(main, port) do
    {:ok, s} = :gen_udp.open(0, @opts)
    right = trips(s, port, 0, 0)
    :gen_udp.close(s)
    send(main, {:right, right})
  end

  defp trips(_s, _port, @k, right), do: right

  defp trips(s, port, j, right) do
    :ok = :gen_udp.send(s, {127, 0, 0, 1}, port, @message)
    {:ok, {_, _, got}} = :gen_udp.recv(s, 0)
    trips(s, port, j + 1, if(got == @message, do: right + 1, else: right))
  end

  def run do
    {:ok, server} = :gen_udp.open(0, @opts)
    {:ok, port} = :inet.port(server)
    pid = spawn(fn -> receive(do: (:go -> serve(server))) end)
    :ok = :gen_udp.controlling_process(server, pid)
    send(pid, :go)
    main = self()
    start = System.monotonic_time(:millisecond)
    for _ <- 1..@c, do: spawn(fn -> client(main, port) end)
    total = collect(@c, 0)
    if total != @c * @k, do: raise("wrong count")
    IO.puts("ms=#{System.monotonic_time(:millisecond) - start}")
  end

  defp collect(0, total), do: total

  defp collect(n, total) do
    receive do
      {:right, r} -> collect(n - 1, total + r)
    end
  end
end

EchoUdp.run()
