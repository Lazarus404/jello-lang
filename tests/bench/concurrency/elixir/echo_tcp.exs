# 50 connections each make 2,000 round trips of 64 bytes through an echo server over loopback.
defmodule EchoTcp do
  @c 50
  @k 2000
  @message "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  @opts [:binary, active: false, nodelay: true]

  def accept(listener) do
    {:ok, s} = :gen_tcp.accept(listener)
    pid = spawn(fn -> receive(do: (:go -> echo(s))) end)
    :ok = :gen_tcp.controlling_process(s, pid)
    send(pid, :go)
    accept(listener)
  end

  def echo(s) do
    case :gen_tcp.recv(s, 0) do
      {:ok, data} ->
        :ok = :gen_tcp.send(s, data)
        echo(s)

      {:error, _} ->
        :gen_tcp.close(s)
    end
  end

  def client(main, port) do
    {:ok, s} = :gen_tcp.connect({127, 0, 0, 1}, port, @opts)
    right = trips(s, 0, 0)
    :gen_tcp.close(s)
    send(main, {:right, right})
  end

  defp trips(_s, @k, right), do: right

  defp trips(s, j, right) do
    :ok = :gen_tcp.send(s, @message)
    {:ok, got} = :gen_tcp.recv(s, 64)
    trips(s, j + 1, if(got == @message, do: right + 1, else: right))
  end

  def run do
    {:ok, listener} = :gen_tcp.listen(0, [{:reuseaddr, true}, {:backlog, 1024} | @opts])
    {:ok, port} = :inet.port(listener)
    spawn(fn -> accept(listener) end)
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

EchoTcp.run()
