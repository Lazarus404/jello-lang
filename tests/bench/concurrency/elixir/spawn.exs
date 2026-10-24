# Spawns 1,000,000 processes, then waits for them all.
defmodule Spawn do
  @n 1_000_000

  def run do
    start = System.monotonic_time(:millisecond)
    main = self()
    spawn_all(main, @n)
    wait(@n)
    IO.puts("ms=#{System.monotonic_time(:millisecond) - start}")
  end

  defp spawn_all(_main, 0), do: :ok

  defp spawn_all(main, k) do
    spawn(fn -> send(main, :finished) end)
    spawn_all(main, k - 1)
  end

  defp wait(0), do: :ok

  defp wait(k) do
    receive do
      :finished -> wait(k - 1)
    end
  end
end

Spawn.run()
