# 50 keep-alive clients each GET "hello world" 400 times from Bandit over loopback, by Mint.
Mix.install([{:bandit, "~> 1.12"}, {:mint, "~> 1.10"}, {:hpax, "~> 1.0.4"}])

defmodule Hello do
  @behaviour Plug
  def init(options), do: options
  def call(conn, _options), do: Plug.Conn.send_resp(conn, 200, "hello world")
end

defmodule HttpHello do
  @c 50
  @k 400

  def client(main, port) do
    {:ok, conn} = Mint.HTTP1.connect(:http, "127.0.0.1", port, mode: :passive)
    {conn, right} = requests(conn, 0, 0)
    Mint.HTTP1.close(conn)
    send(main, {:right, right})
  end

  defp requests(conn, @k, right), do: {conn, right}

  defp requests(conn, j, right) do
    {:ok, conn, ref} = Mint.HTTP1.request(conn, "GET", "/", [], nil)
    {conn, status, body} = response(conn, ref, nil, "")
    requests(conn, j + 1, if(status == 200 and body == "hello world", do: right + 1, else: right))
  end

  defp response(conn, ref, status, body) do
    {:ok, conn, parts} = Mint.HTTP1.recv(conn, 0, 5000)

    Enum.reduce(parts, {conn, status, body, false}, fn
      {:status, ^ref, s}, {c, _, b, d} -> {c, s, b, d}
      {:data, ^ref, more}, {c, s, b, d} -> {c, s, b <> more, d}
      {:done, ^ref}, {c, s, b, _} -> {c, s, b, true}
      _, acc -> acc
    end)
    |> case do
      {conn, status, body, true} -> {conn, status, body}
      {conn, status, body, false} -> response(conn, ref, status, body)
    end
  end

  def run do
    {:ok, server} = Bandit.start_link(plug: Hello, ip: {127, 0, 0, 1}, port: 0, startup_log: false)
    {:ok, {_, port}} = ThousandIsland.listener_info(server)
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

HttpHello.run()
