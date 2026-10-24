# IDLE_N processes (default 100,000) all parked in receive at once; the
# harness reads the peak resident memory.
defmodule Idle do
  def run do
    n = String.to_integer(System.get_env("IDLE_N") || "100000")
    start = System.monotonic_time(:millisecond)
    main = self()

    parked =
      for _ <- 1..n//1 do
        spawn(fn ->
          send(main, :ready)

          receive do
            :stop -> send(main, :stopped)
          end
        end)
      end

    wait(n, :ready)
    for p <- parked, do: send(p, :stop)
    wait(n, :stopped)
    IO.puts("ms=#{System.monotonic_time(:millisecond) - start}")
  end

  defp wait(0, _), do: :ok

  defp wait(k, message) do
    receive do
      ^message -> wait(k - 1, message)
    end
  end
end

Idle.run()
