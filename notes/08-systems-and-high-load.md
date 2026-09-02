# 08 — Systems programming and high-load architecture

> This is the note that answers "what does it actually take to handle 100k
> connections", and why the answer is architectural rather than a matter of
> writing faster code.

---

## The C10K problem, and why it reshaped everything

In 1999 Dan Kegel asked: how do you handle 10,000 simultaneous connections?
The naive answer — one thread per connection — fails, and the arithmetic shows
why:

```
10,000 threads × 512 KB stack (macOS)   = 5 GB of stack address space
10,000 threads × 8 MB stack (Linux)     = 80 GB
kernel scheduler run queue              = 10,000 entries to manage
context switch                          = ~1-3 µs each, PLUS a cold cache after
```
The machine spends its cycles *switching between* threads instead of doing work.
Throughput collapses long before memory runs out.

**The fix, universal since ~2004: readiness-based multiplexing.**
- every socket non-blocking
- one syscall asks the kernel "which of these N fds are ready?"
- handle exactly those, never blocking
- a handful of threads (typically one per core), not N

Memory per connection drops from ~512 KB of stack to ~10 KB of buffers. You get
100k+ connections on one box.

You built this in `08-systems/03-kqueue-event-loop.cpp`. It's ~250 lines.

---

## The readiness APIs

| API | Platform | Complexity | Notes |
|---|---|---|---|
| `select()` | POSIX | O(n) scan | 1024-fd limit. Obsolete. |
| `poll()` | POSIX | O(n) scan | no limit; portable fallback |
| **`epoll`** | Linux | O(ready) | the production answer on Linux |
| **`kqueue`** | BSD/macOS | O(ready) | same model, also handles timers, signals, file events, process events — a nicer API |
| `IOCP` | Windows | completion-based | you submit ops, get completions |
| **`io_uring`** | Linux 5.1+ | completion-based | shared-memory submission/completion rings; batches many ops per syscall, sometimes **zero** syscalls in steady state. State of the art. |

**Readiness** ("this socket can be read now") vs **completion** ("your read
finished, here's the data"). Completion models map better to async/await and
avoid a syscall per operation. io_uring is Linux moving to the completion model,
and it's why it's a big deal — it can eliminate the syscall from the hot path
entirely.

### Level-triggered vs edge-triggered
- **Level-triggered** (default): "this fd is ready" is reported every time you
  ask, while it stays ready. Forgiving. Use this first.
- **Edge-triggered** (`EPOLLET` / `EV_CLEAR`): reported only on *change*. Fewer
  wakeups, but you **must** drain to `EAGAIN` or you hang forever waiting for an
  edge that already happened. This is a classic production bug.

---

## Counting syscalls: the discipline

A syscall costs ~100ns-1µs (worse since Spectre/Meltdown mitigations). Measured
in `08-systems/01`: 20,000 one-byte writes took 44,791 µs; the same 20,000 bytes
in one buffered write took **9 µs**. That's ~5000x.

A naive HTTP server per request: `accept`, `read`, `read`, `write`, `write`,
`close` = 6+ syscalls ≈ 3µs of pure kernel overhead before any of your code
runs. At 100k rps that's ~30% of a core, wasted.

**How real servers cut it:**
- userspace buffering (one syscall per buffer, not per message)
- `writev`/`sendmsg` — send header + body in one call, no concatenation
- `sendfile`/`splice` — file→socket with **no userspace copy at all**
- `accept4()` — set `O_NONBLOCK|O_CLOEXEC` without extra `fcntl` calls
- epoll/kqueue — one readiness call for 10,000 sockets
- io_uring — batch dozens of operations per syscall, or none

Measure yours: `strace -c ./prog` (Linux) or `dtruss` (macOS). Do this once on
any server you own; the number is usually shocking.

---

## Architecture patterns, ranked by scalability

### 1. Thread per connection, blocking
`08-systems/02`. Simple, debuggable, correct. **Fine up to a few hundred
connections doing CPU-heavy work.** Don't build an event loop you don't need.

### 2. Thread pool + blocking I/O
Bounded threads, queued requests. Better, but a slow request still occupies a
whole thread, and you need queue depth ≈ concurrency.

### 3. Single-threaded event loop
Redis, Node.js. One core, no locks, no races, trivially reasoned about. **Never
block it** — one blocking call stalls every connection.

### 4. Event loop per core (`SO_REUSEPORT`)
Each core runs its own accept loop and event loop, with its own listening socket
on the same port; the kernel load-balances incoming connections. nginx does
this. **No shared state, no locks, near-linear scaling.**

### 5. Thread-per-core / shared-nothing
Seastar, ScyllaDB, DPDK, ClickHouse. Pin one thread per core; shard the *data*
by core; communicate between cores with explicit lock-free message queues; never
share mutable state. Combine with:
- per-core memory allocators (no allocator lock)
- per-core arenas, reset per request
- pinned threads, huge pages, sometimes kernel bypass

This is the architecture behind every "millions of requests per second on one
box" claim you've read.

### The general principle
> **Scaling comes from removing sharing, not from adding threads.**

---

## Framing: TCP is a byte stream, not a message stream

The #1 beginner networking bug. Your 100-byte `send` may arrive as 40 + 60, or
be merged with the next one. TCP guarantees **order and reliability, not
boundaries.**

You must define boundaries yourself:
- **length prefix** — 4 bytes of length, then the payload. Best for binary.
  Always bound the length before allocating, or a malicious 4 GB length is a
  trivial DoS.
- **delimiter** — HTTP's `\r\n\r\n`. Needs escaping if the payload can contain it.
- **fixed size** — simplest, wasteful.
- **self-describing** — protobuf, etc.

And always: **buffer partial messages** across reads. A message may arrive in
pieces across many `recv` calls.

---

## Backpressure — the thing that kills async systems

An event loop can accept work far faster than it can complete it. Without
limits:
```
slow consumer → queue grows → memory grows → OOM kill at 3am
```
This is the single most common way async systems die.

Bound everything:
- bounded queues (block or reject, don't grow)
- max connections; max in-flight requests
- max buffer size per connection (`08-systems/03` disconnects at 1 MB)
- stop reading from a socket when the peer isn't draining your writes
- **load shedding**: reject early with 503 rather than queueing forever. A
  request that times out anyway was pure waste.

Little's Law: `concurrency = arrival_rate × latency`. If latency rises and
arrivals don't fall, concurrency (and memory) rises without bound. Bound one of
them explicitly or the machine will bound it for you.

---

## The socket options that matter

```cpp
SO_REUSEADDR   // restart without waiting out TIME_WAIT. Always set.
SO_REUSEPORT   // N processes/threads bind the SAME port; kernel balances. Key
               // to the event-loop-per-core architecture.
TCP_NODELAY    // disable Nagle. Nagle + delayed ACK = up to 40ms of latency on
               // request/response protocols. Every RPC/HTTP server sets this.
SO_RCVBUF/SNDBUF  // kernel buffer sizes; matters on high bandwidth-delay paths
SO_KEEPALIVE   // detect dead peers (default probe interval is ~2 HOURS; tune it)
SO_LINGER      // control close() behaviour
TCP_QUICKACK   // (Linux) disable delayed ACK
```

## The errno values you must handle
```
EAGAIN/EWOULDBLOCK  "not ready" — NOT an error. The whole basis of async I/O.
EINTR               a signal interrupted the call — just retry.
EPIPE / ECONNRESET  peer went away. Normal. Don't log it as an error.
EMFILE / ENFILE     out of file descriptors. An emergency: you can no longer
                    accept anything. Real servers keep a spare fd reserved so
                    they can accept-and-immediately-close gracefully.
EADDRINUSE          you forgot SO_REUSEADDR.
```

---

## What you still need for a production server

Your event loop is missing:
- **Timeouts.** Idle connections must be reaped. Use a **timing wheel** (O(1)
  insert/expire) rather than a heap of timers.
- **Graceful shutdown.** Stop accepting, drain in-flight, then exit.
- **TLS.** OpenSSL/BoringSSL. Note it adds a *state machine on top of* your
  state machine — `SSL_read` may need a *write* to make progress.
- **HTTP parsing.** Use `llhttp` (Node's parser) or `picohttpparser`. Do not
  hand-roll it; the edge cases are a security surface.
- **DNS.** `getaddrinfo` **blocks**. Never call it on an event loop thread. Use
  c-ares or a resolver thread.
- **File I/O.** Regular files are *always* "ready" to epoll, so it can't help.
  Use a thread pool, or io_uring, which handles files properly.
- **Metrics, structured logging, tracing** — per-core counters, aggregated.
- **Limits**: `ulimit -n` (fds), `somaxconn`, ephemeral port range, conntrack.

---

## Books and code worth your time

**Books**
- *The Linux Programming Interface*, Kerrisk — the definitive syscall reference.
- *Unix Network Programming vol.1*, Stevens — sockets, exhaustively.
- *Computer Systems: A Programmer's Perspective* — the machine, end to end.
- *Systems Performance*, Gregg — methodology and tooling.

**Code to read** (small → large)
- `redis` (C) — a beautiful, tiny, single-threaded event loop (`ae.c`)
- `libuv` (C) — the cross-platform loop under Node
- `picohttpparser` — 500 lines, SIMD HTTP parsing
- `simdjson` — what SIMD can really do
- `seastar` (C++) — the thread-per-core future
- `folly` (C++) — Facebook's production toolbox
- `nginx` (C) — 20 years of production hardening

Next: `notes/09-building-a-runtime.md`.
