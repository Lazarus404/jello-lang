# 200,000 request/reply round trips between two processes.
defmodule Call do
  @n 200_000

  def server do
    receive do
      {:add, n, from, ref} -> send(from, {ref, n + 1})
    end

    server()
  end

  def run do
    server = spawn(&server/0)
    start = System.monotonic_time(:millisecond)
    right = loop(server, 0, 0)
    if right != @n, do: raise("wrong reply")
    IO.puts("ms=#{System.monotonic_time(:millisecond) - start}")
  end

  defp loop(_server, @n, right), do: right

  defp loop(server, i, right) do
    ref = make_ref()
    send(server, {:add, i, self(), ref})

    reply =
      receive do
        {^ref, value} -> value
      end

    loop(server, i + 1, if(reply == i + 1, do: right + 1, else: right))
  end
end

Call.run()
