# Distributed KV Cache

## Build

```sh
make
```

This builds the server (`kv_cache`) and the interactive client (`kv_client`).

## Start a cluster

Start one process per node. Every process must receive the exact same complete
node list, and each process's `--self` endpoint must appear in that list. Use
each node's unique, reachable address as its endpoint; do not use `127.0.0.1`
for a node that other machines must contact. The endpoint text must be identical
on every node (for example, don't list a node by its public IP on one server and
by its private IP or `localhost` on another):

```sh
./kv_cache --self 127.0.0.1:9001 --nodes 127.0.0.1:9001,127.0.0.1:9002,127.0.0.1:9003
./kv_cache --self 127.0.0.1:9002 --nodes 127.0.0.1:9001,127.0.0.1:9002,127.0.0.1:9003
./kv_cache --self 127.0.0.1:9003 --nodes 127.0.0.1:9001,127.0.0.1:9002,127.0.0.1:9003
```

Run each command in a separate terminal or on its corresponding host. The server
binds to all local interfaces on the port from `--self`; the endpoint hostname is
the address other nodes and clients use to reach it. Data is persisted to a
`cache_<port>.log` file in the server's working directory.

The server uses a deterministic FNV-1a hash ring with virtual nodes to assign
each key to one owner. Requests can be sent to any node; non-owner nodes forward
them to the owner. A forwarding hop limit returns an error instead of allowing
inconsistent node lists to create a routing loop. This provides key distribution,
not replication or failover: a request fails if its owner is down.

## Use the client

Connect to any node, then enter one command per line:

```sh
./kv_client 127.0.0.1 9001
SET color blue
GET color
DEL color
exit
```

The supported commands are `SET key value`, `GET key`, and `DEL key`. Keys and
values are single tokens (no whitespace). Responses are `OK`, `VALUE <value>`,
`NOT_FOUND`, or `ERROR <message>`. `kv_client` keeps the TCP connection open
until `exit`, `quit`, or end-of-input.

## Benchmark

The Python benchmark opens persistent client connections, sends a configurable
mix of SET and GET operations, and reports throughput and latency percentiles:

```sh
python3 scripts/benchmark.py --host 127.0.0.1 --port 9001 \
  --requests 100000 --concurrency 32 --key-space 10000
```

Use `python3 scripts/benchmark.py --help` to see all options. The benchmark
requires the Python 3 standard library only.
