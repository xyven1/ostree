"""Deterministic stand-in for a remote object store, for benchmarking.

Serves files under ROOT over plain HTTP/1.1 keep-alive with:
  --latency      seconds added before each response (request round trip)
  --connect      seconds added once per new connection (TCP + TLS setup)
  --conn-rate    bytes/s cap per connection
  --total-rate   bytes/s cap across all connections
Supports GET with single-range "Range: bytes=a-b". Ignores all auth headers.
GET /__stats returns counters as text and resets them.
"""
import argparse
import asyncio
import os
import time

args = None
trace = {"active": 0, "bytes": 0, "reqs": 0, "big": 0}
stats = {"requests": 0, "connections": 0, "bytes": 0, "ranged": 0, "open": 0, "max_open": 0, "notfound": 0}
global_next = 0.0


async def pace(state, n):
    """Sleep so that this connection and the whole server stay under their caps."""
    global global_next
    now = time.monotonic()
    wake = now
    if args.conn_rate:
        state["next"] = max(now, state["next"]) + n / args.conn_rate
        wake = max(wake, state["next"])
    if args.total_rate:
        global_next = max(now, global_next) + n / args.total_rate
        wake = max(wake, global_next)
    if wake > now:
        await asyncio.sleep(wake - now)


async def handle(reader, writer):
    stats["connections"] += 1
    stats["open"] += 1
    stats["max_open"] = max(stats["max_open"], stats["open"])
    state = {"next": 0.0}
    try:
        if args.connect:
            await asyncio.sleep(args.connect)
        while True:
            try:
                head = await reader.readuntil(b"\r\n\r\n")
            except (asyncio.IncompleteReadError, ConnectionError):
                return
            lines = head.decode("latin-1").split("\r\n")
            method, target, _ = lines[0].split(" ", 2)
            headers = {}
            for line in lines[1:]:
                if ":" in line:
                    k, v = line.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
            if args.latency:
                await asyncio.sleep(args.latency)

            path = target.split("?", 1)[0]
            if path == "/__stats":
                body = (" ".join(f"{k}={v}" for k, v in stats.items()) + "\n").encode()
                for k in ("requests", "connections", "bytes", "ranged", "max_open", "notfound"):
                    stats[k] = 0
                stats["max_open"] = stats["open"]
                writer.write(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
                await writer.drain()
                continue

            stats["requests"] += 1
            # percent-decoding: only what ostree paths need
            rel = path.lstrip("/").replace("%2B", "+").replace("%2b", "+")
            full = os.path.normpath(os.path.join(args.root, rel))
            if not full.startswith(args.root) or not os.path.isfile(full) or method != "GET":
                stats["notfound"] += 1
                body = b"<Error><Code>NoSuchKey</Code><Message>not found</Message></Error>"
                writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
                await writer.drain()
                continue

            size = os.path.getsize(full)
            start, end, status = 0, size - 1, b"200 OK"
            extra = b""
            rng = headers.get("range")
            if rng and rng.startswith("bytes=") and size > 0:
                a, b = rng[6:].split("-", 1)
                start = int(a) if a else max(0, size - int(b))
                end = min(int(b), size - 1) if (a and b) else size - 1
                if start > end or start >= size:
                    writer.write(b"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%d\r\nContent-Length: 0\r\n\r\n" % size)
                    await writer.drain()
                    continue
                status = b"206 Partial Content"
                extra = b"Content-Range: bytes %d-%d/%d\r\n" % (start, end, size)
                stats["ranged"] += 1
            length = end - start + 1
            trace["active"] += 1
            trace["reqs"] += 1
            is_big = length >= 1048576
            if is_big:
                trace["big"] += 1
            writer.write(b"HTTP/1.1 " + status + b"\r\nContent-Length: %d\r\nAccept-Ranges: bytes\r\n" % length
                         + extra + b"ETag: \"%d-%d\"\r\n\r\n" % (size, int(os.path.getmtime(full))))
            throttled = bool(args.conn_rate or args.total_rate)
            chunk = 16384 if throttled else 262144
            with open(full, "rb") as f:
                f.seek(start)
                left = length
                while left > 0:
                    data = f.read(min(chunk, left))
                    if not data:
                        break
                    if throttled:
                        await pace(state, len(data))
                    writer.write(data)
                    await writer.drain()
                    left -= len(data)
                    stats["bytes"] += len(data)
                    trace["bytes"] += len(data)
            trace["active"] -= 1
            if is_big:
                trace["big"] -= 1
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        stats["open"] -= 1
        try:
            writer.close()
        except Exception:
            pass


async def tracer():
    t0 = time.monotonic()
    with open(args.trace, "w") as f:
        while True:
            await asyncio.sleep(1.0)
            f.write("%5.0f open=%3d active=%3d big=%3d reqs=%5d MiB=%5.1f\n" % (
                time.monotonic() - t0, stats["open"], trace["active"], trace["big"], trace["reqs"], trace["bytes"] / 1048576))
            f.flush()
            trace["bytes"] = 0
            trace["reqs"] = 0


async def main():
    if args.trace:
        asyncio.ensure_future(tracer())
    server = await asyncio.start_server(handle, "127.0.0.1", args.port, backlog=1024)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("root")
    p.add_argument("--port", type=int, default=18080)
    p.add_argument("--latency", type=float, default=0.0)
    p.add_argument("--connect", type=float, default=0.0)
    p.add_argument("--conn-rate", type=float, default=0.0)
    p.add_argument("--total-rate", type=float, default=0.0)
    p.add_argument("--trace", default="")
    args = p.parse_args()
    args.root = os.path.realpath(args.root)
    asyncio.run(main())
