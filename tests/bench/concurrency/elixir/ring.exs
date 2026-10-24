# 1,000 processes in a ring pass a token for 1,000 laps.
defmodule Ring do
  @n 1000
  @laps 1000

  def member(main) do
    receive do
      {:link, next} -> loop(main, next, 0)
    end
  end

  defp loop(_main, _next, @laps), do: :ok

  defp loop(main, next, k) do
    receive do
      {:token, 1} -> send(main, {:done, k})
      {:token, hops} -> send(next, {:token, hops - 1})
    end

    loop(main, next, k + 1)
  end

  def run do
    start = System.monotonic_time(:millisecond)
    main = self()
    members = for _ <- 1..@n, do: spawn(fn -> member(main) end)
    tuple = List.to_tuple(members)
    for i <- 0..(@n - 1), do: send(elem(tuple, i), {:link, elem(tuple, rem(i + 1, @n))})
    send(elem(tuple, 0), {:token, @n * @laps})
    expected = @laps - 1

    receive do
      {:done, ^expected} -> :ok
    end

    IO.puts("ms=#{System.monotonic_time(:millisecond) - start}")
  end
end

Ring.run()
