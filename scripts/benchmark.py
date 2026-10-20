#!/usr/bin/env python3
"""Benchmark the newline-delimited key-value server protocol."""

import argparse
import concurrent.futures
import math
import socket
import statistics
import string
import time
import random


def run_worker(args, worker_id, request_count):
    latencies = []
    errors = []
    rng = random.Random(worker_id)
    alphabet = string.ascii_letters + string.digits

    try:
        with socket.create_connection((args.host, args.port), timeout=10) as connection:
            connection.settimeout(10)
            with connection.makefile("rwb") as stream:
                for request_id in range(request_count):
                    key_index = rng.randrange(args.key_space)
                    key = f"bench_{key_index}"
                    if rng.random() < args.set_ratio:
                        value = "".join(rng.choices(alphabet, k=args.value_size))
                        command = f"SET {key} {value}"
                    else:
                        command = f"GET {key}"

                    started = time.perf_counter()
                    stream.write(command.encode("ascii") + b"\n")
                    stream.flush()
                    response = stream.readline()
                    elapsed = time.perf_counter() - started
                    if not response:
                        errors.append("server closed the connection")
                        break
                    if response.startswith(b"ERROR "):
                        errors.append(response.decode("utf-8", errors="replace").strip())
                        break
                    if response not in (b"OK\n", b"NOT_FOUND\n") and not response.startswith(b"VALUE "):
                        errors.append(
                            f"unexpected response: {response.decode('utf-8', errors='replace').strip()}"
                        )
                        break
                    latencies.append(elapsed)
    except (OSError, UnicodeError) as error:
        errors.append(str(error))

    return latencies, errors


def percentile(sorted_values, percentage):
    if not sorted_values:
        return 0.0
    index = max(0, math.ceil(percentage * len(sorted_values)) - 1)
    return sorted_values[index] * 1000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="server address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9001, help="server port (default: 9001)")
    parser.add_argument("--requests", type=int, default=10000, help="total operations to send")
    parser.add_argument("--concurrency", type=int, default=32, help="number of client connections")
    parser.add_argument("--key-space", type=int, default=10000, help="number of distinct keys")
    parser.add_argument("--value-size", type=int, default=64, help="SET value length in bytes")
    parser.add_argument(
        "--set-ratio",
        type=float,
        default=0.7,
        help="fraction of operations that are SET (default: 0.7; remainder are GET)",
    )
    args = parser.parse_args()

    if args.requests <= 0 or args.concurrency <= 0 or args.key_space <= 0 or args.value_size <= 0:
        parser.error("--requests, --concurrency, --key-space, and --value-size must be positive")
    if not 0 <= args.set_ratio <= 1:
        parser.error("--set-ratio must be between 0 and 1")

    worker_counts = [
        args.requests // args.concurrency + (worker_id < args.requests % args.concurrency)
        for worker_id in range(args.concurrency)
    ]
    started = time.perf_counter()
    latencies = []
    errors = []

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
            futures = [
                executor.submit(run_worker, args, worker_id, count)
                for worker_id, count in enumerate(worker_counts)
                if count
            ]
            for future in concurrent.futures.as_completed(futures):
                worker_latencies, worker_errors = future.result()
                latencies.extend(worker_latencies)
                errors.extend(worker_errors)
    except OSError as error:
        parser.error(f"could not start benchmark clients: {error}")

    elapsed = time.perf_counter() - started
    latencies.sort()
    completed = len(latencies)
    print(f"Target: {args.host}:{args.port}")
    print(f"Requested operations: {args.requests:,}")
    print(f"Completed operations: {completed:,}")
    print(f"Errors: {len(errors):,}")
    print(f"Elapsed: {elapsed:.3f} s")
    print(f"Throughput: {completed / elapsed:,.0f} ops/s")
    if latencies:
        print(f"Latency avg: {statistics.mean(latencies) * 1000:.3f} ms")
        print(f"Latency p50: {percentile(latencies, 0.50):.3f} ms")
        print(f"Latency p95: {percentile(latencies, 0.95):.3f} ms")
        print(f"Latency p99: {percentile(latencies, 0.99):.3f} ms")
    for error in errors[:10]:
        print(f"Error detail: {error}")
    if errors or completed != args.requests:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
