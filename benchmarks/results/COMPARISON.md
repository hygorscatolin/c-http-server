# TCP_NODELAY: before and after

Two runs of `scripts/benchmark.sh`, same machine, same 10s per scenario,
8 workers on 8 CPUs. The only difference between them is the
`set_tcp_nodelay()` call added to `accept_new_connections()` in
`src/event_loop.c`.

- **before:** `20260804-153317/` — Nagle enabled (kernel default)
- **after:** `20260804-154353/` — `TCP_NODELAY` set on every accepted socket

## small-file (`/hello.txt`, 271 bytes, served with sendfile)

The scenario the change was aimed at.

| conn | req/s before | req/s after | avg before | avg after | p99 before | p99 after |
|-----:|-------------:|------------:|-----------:|----------:|-----------:|----------:|
|    1 |           22 |       5,894 |    44.64ms |  **0.24ms** |    48.12ms |   2.05ms |
|   10 |          179 |      20,933 |    44.62ms |    1.44ms |    48.24ms |  22.41ms |
|   50 |        1,066 |      66,413 |    44.89ms |    1.16ms |    55.99ms |  13.15ms |
|  200 |        4,457 |      69,454 |    44.63ms |    2.94ms |    56.11ms |  15.57ms |

At one connection that is 268x the throughput and 186x lower latency.
The 44ms floor is gone at every concurrency level, which is the point:
it was never contention, it was the delayed-ACK timer, so it did not
move with load and no amount of concurrency could amortize it away.

The `inline` scenario (`/`, whole response in a single `write()`) is the
control, and small-file now sits right next to it, as it should — the
two paths differ only in where the body comes from:

| conn | inline avg | small-file avg (after) |
|-----:|-----------:|-----------------------:|
|    1 |     0.15ms |                 0.24ms |
|   50 |     1.17ms |                 1.16ms |

## large-file (8MiB, served with sendfile)

Unchanged, as expected: Nagle never holds back a full-size segment, so a
multi-megabyte body was never subject to the stall. The two runs happen
to disagree (781 vs 388 req/s at c10), which is run-to-run variance on a
loopback benchmark competing with wrk for the same cores, not an effect
of the change. Confirmed by running the two binaries alternately, three
rounds each at c10:

| round | TCP_NODELAY | no TCP_NODELAY |
|------:|------------:|---------------:|
|     1 |      796.93 |         807.48 |
|     2 |      805.50 |         802.16 |
|     3 |      850.44 |         809.32 |

Same population. The large-file rows in the two saved runs should be read
as noise, and the ~800 req/s here is the better estimate for both.

## Caveat

These are loopback numbers with the load generator on the same 8 CPUs as
the server, so treat the columns as relative to each other, not as
absolute capacity. See the header comment in `scripts/benchmark.sh`.
