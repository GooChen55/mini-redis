"""Example: python3 tools/benchmark.py --requests 20000 --clients 8 --pipeline 32"""
import argparse
import concurrent.futures
import math
import statistics
import threading
import time
from resp_client import Client


def main():
    parser = argparse.ArgumentParser(description="Mini Redis benchmark (pipeline batch latency)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6379)
    parser.add_argument("--requests", type=int, default=20000)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--pipeline", type=int, default=32)
    parser.add_argument("--value-size", type=int, default=64)
    parser.add_argument("--command", choices=["PING", "SET", "GET", "INCR"], default="PING")
    args = parser.parse_args()
    if min(args.requests, args.clients, args.pipeline, args.value_size) < 1 or args.clients > args.requests:
        parser.error("sizes must be positive, clients must not exceed requests")
    value = b"x" * args.value_size
    prefix = f"bench:{time.time_ns()}:"
    with Client(args.host, args.port) as c:
        for i in range(args.clients):
            if args.command == "GET":
                c.command("SET", prefix + str(i), value)
    barrier = threading.Barrier(args.clients + 1)

    def worker(index):
        remaining = args.requests // args.clients + (index < args.requests % args.clients)
        latencies = []
        with Client(args.host, args.port) as c:
            key = prefix + str(index)
            command = {"PING": ("PING",), "SET": ("SET", key, value),
                       "GET": ("GET", key), "INCR": ("INCR", key)}[args.command]
            # Warm each connection before the timed phase.
            c.command("PING")
            barrier.wait(timeout=30)
            while remaining:
                count = min(remaining, args.pipeline)
                start = time.perf_counter()
                replies = c.pipeline([command] * count)
                expected = {"PING": b"PONG", "SET": b"OK", "GET": value}.get(args.command)
                if args.command == "INCR":
                    if any(not isinstance(reply, int) for reply in replies):
                        raise RuntimeError("unexpected INCR reply")
                elif any(reply != expected for reply in replies):
                    raise RuntimeError("unexpected reply")
                latencies.append((time.perf_counter() - start) * 1000)
                remaining -= count
        return latencies

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as pool:
        futures = [pool.submit(worker, i) for i in range(args.clients)]
        barrier.wait(timeout=30)
        start = time.perf_counter()
        samples = [sample for future in futures for sample in future.result()]
        elapsed = time.perf_counter() - start
    samples.sort()
    percentile = lambda p: samples[max(0, math.ceil(len(samples) * p) - 1)]
    print(f"{args.command}: {args.requests} requests, {args.clients} clients, pipeline={args.pipeline}")
    print(f"elapsed={elapsed:.3f}s throughput={args.requests / elapsed:,.0f} requests/s")
    print(f"batch round-trip latency: mean={statistics.mean(samples):.3f}ms "
          f"p50={percentile(.50):.3f}ms p95={percentile(.95):.3f}ms p99={percentile(.99):.3f}ms")
    # Delete only this run's benchmark keys.
    with Client(args.host, args.port) as c:
        for start_index in range(0, args.clients, 1023):
            c.command("DEL", *(prefix + str(i) for i in range(start_index, min(start_index + 1023, args.clients))))


if __name__ == "__main__":
    main()
