# One sender sends 100 messages to each of 10,000 receivers.
defmodule Fanout do
  @r 10_000
  @m 100

  def receiver(main, sum, @m), do: send(main, {:sum, sum})

  def receiver(main, sum, k) do
    receive do
      v -> receiver(main, sum + v, k + 1)
    end
  end

  def run do
    start = System.monotonic_time(:millisecond)
    main = self()
    receivers = for _ <- 1..@r, do: spawn(fn -> receiver(main, 0, 0) end)
    Enum.each(0..(@m - 1), fn k -> Enum.each(receivers, &send(&1, k)) end)
    total = collect(@r, 0)
    if total != @r * div(@m * (@m - 1), 2), do: raise("wrong total")
    IO.puts("ms=#{System.monotonic_time(:millisecond) - start}")
  end

  defp collect(0, total), do: total

  defp collect(k, total) do
    receive do
      {:sum, s} -> collect(k - 1, total + s)
    end
  end
end

Fanout.run()
