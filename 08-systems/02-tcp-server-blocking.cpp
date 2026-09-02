// ===========================================================================
//  A TCP SERVER, THE BLOCKING / THREAD-PER-CONNECTION WAY
//  run: make run F=08-systems/02-tcp-server-blocking.cpp
//       (it starts a server, drives it with its own clients, and exits)
//
//  Or run it as a real server and talk to it yourself:
//       ./build/08-systems/02-tcp-server-blocking serve 9000
//       nc localhost 9000
// ===========================================================================
//
//  ⚠️ THERE IS NO NETWORKING IN THE C++ STANDARD LIBRARY.
//  Not in C++23. (Networking TS based on Boost.Asio was proposed for C++20 and
//  did not make it; the committee is now pursuing a sender/receiver-based
//  design.) So in C++ you either:
//     - use a library (Boost.Asio, libuv, POCO, cpp-httplib, seastar), or
//     - call the POSIX socket API directly, which is what this file does.
//
//  You asked to go deeper. This IS the deep part: the exact API that Netty,
//  nginx, and the JVM's NIO all sit on top of.
//
//  ARCHITECTURE HERE: one thread per connection, blocking I/O.
//  This is what a naive Java server (or Tomcat's classic BIO connector) does.
//  It is simple, correct, easy to debug — and it does not scale past a few
//  thousand connections. 03-kqueue-event-loop.cpp shows the fix.
//
// ===========================================================================

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>       // inet_ntop, htons
#include <netinet/in.h>      // sockaddr_in
#include <netinet/tcp.h>     // TCP_NODELAY
#include <sys/socket.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// The same RAII fd wrapper from file 01. In a real project this lives in a
// shared header — every socket, file, and pipe in your program uses it.
// ---------------------------------------------------------------------------
class Fd {
    int fd_ = -1;
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) ::close(fd_); fd_ = std::exchange(o.fd_, -1); }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
};

[[noreturn]] static void throw_errno(const char* what) {
    throw std::system_error(errno, std::system_category(), what);
}

// ===========================================================================
//  CREATING A LISTENING SOCKET — the five-step ritual
// ===========================================================================
static Fd make_listener(std::uint16_t port, int backlog = 128) {
    // (1) socket(): create an endpoint.
    //     AF_INET     = IPv4 (AF_INET6 for IPv6; use AF_INET6 + dual-stack in
    //                   production so you serve both)
    //     SOCK_STREAM = reliable, ordered byte stream = TCP
    //     0           = default protocol for that type
    Fd fd{::socket(AF_INET, SOCK_STREAM, 0)};
    if (!fd) throw_errno("socket");

    // (2) setsockopt(SO_REUSEADDR): without this, restarting your server fails
    //     with "Address already in use" for up to 2*MSL (~60s) because the old
    //     socket sits in TIME_WAIT. Every server sets this. Every developer
    //     has been confused by not setting it exactly once.
    int yes = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
        throw_errno("setsockopt SO_REUSEADDR");

    // (3) bind(): claim the address and port.
    //     ⚠️ NETWORK BYTE ORDER IS BIG-ENDIAN. Your CPU is little-endian.
    //     htons = "host to network short". Forgetting it means you bind to a
    //     byte-swapped port and spend an hour confused. This is why the byte
    //     order discussion in 01-fundamentals/09-pointers.cpp matters.
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 0.0.0.0 = all interfaces
    addr.sin_port        = htons(port);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw_errno("bind");

    // (4) listen(): become a passive socket and create the accept queue.
    //     `backlog` bounds the queue of completed-but-not-yet-accepted
    //     connections. Too small under burst = clients get connection refused
    //     or long latency. Linux also has net.core.somaxconn capping it.
    if (::listen(fd.get(), backlog) < 0) throw_errno("listen");

    // (5) accept() in a loop — done by the caller.
    return fd;
}

// ---------------------------------------------------------------------------
// Read/write helpers that respect the three rules (short I/O, EINTR, EOF).
// ---------------------------------------------------------------------------
static bool write_all(int fd, const char* p, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(fd, p + sent, n - sent, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            // EPIPE: the peer closed. Common and NOT worth logging as an error.
            return false;
        }
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

// ===========================================================================
//  THE PER-CONNECTION HANDLER
//  In this model this function OWNS a whole thread for the whole life of the
//  connection — including all the time the client spends thinking.
// ===========================================================================
static void handle_connection(Fd conn, std::atomic<int>& active) {
    struct Counter {                       // RAII again: decrement on any exit
        std::atomic<int>& a;
        explicit Counter(std::atomic<int>& x) : a(x) { ++a; }
        ~Counter() { --a; }
    } counter(active);

    // TCP_NODELAY disables Nagle's algorithm. Nagle buffers small writes to
    // avoid flooding the network with tiny packets — great for telnet in 1984,
    // terrible for a request/response protocol, where it interacts with delayed
    // ACK to add up to 40ms of latency. EVERY RPC/HTTP server sets this.
    int yes = 1;
    ::setsockopt(conn.get(), IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    char buf[4096];
    while (true) {
        ssize_t n = ::recv(conn.get(), buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;                                  // real error
        }
        if (n == 0) break;                          // peer closed cleanly (FIN)

        // Echo it back, uppercased, so you can see the server did something.
        for (ssize_t i = 0; i < n; ++i)
            buf[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(buf[i])));
        if (!write_all(conn.get(), buf, static_cast<std::size_t>(n))) break;
    }
    // conn's destructor calls close(). The FIN goes out. No leak possible.
}

// ===========================================================================
//  THE ACCEPT LOOP
// ===========================================================================
static void run_server(std::uint16_t port, std::atomic<bool>& stop,
                       std::atomic<int>& active, std::atomic<int>& total) {
    Fd listener = make_listener(port);
    std::cout << "  [server] listening on port " << port << "\n";

    std::vector<std::thread> threads;
    while (!stop.load(std::memory_order_relaxed)) {
        sockaddr_in peer{};
        socklen_t   peer_len = sizeof(peer);

        // accept() BLOCKS until a connection arrives. It returns a NEW fd for
        // that connection; the listening fd stays open for the next one.
        int c = ::accept(listener.get(), reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (c < 0) {
            if (errno == EINTR) continue;
            if (stop.load()) break;
            // EMFILE = out of file descriptors. In production this is a real
            // emergency: you can no longer accept ANY connection. Real servers
            // keep a spare fd open specifically so they can accept-and-close
            // gracefully instead of spinning here.
            if (errno == EMFILE || errno == ENFILE) {
                std::cerr << "  [server] OUT OF FILE DESCRIPTORS\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            break;
        }
        ++total;

        // ⭐ THE COST OF THIS LINE IS THE WHOLE LESSON:
        // one std::thread = one kernel thread = 512 KB of stack (macOS default)
        // + kernel bookkeeping + a context switch every time it blocks.
        threads.emplace_back(handle_connection, Fd{c}, std::ref(active));
    }

    for (auto& t : threads) if (t.joinable()) t.join();
    std::cout << "  [server] stopped\n";
}

// ===========================================================================
//  A CLIENT, so this file can test itself
// ===========================================================================
static std::string client_roundtrip(std::uint16_t port, const std::string& msg) {
    Fd fd{::socket(AF_INET, SOCK_STREAM, 0)};
    if (!fd) throw_errno("socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
        throw std::runtime_error("inet_pton");

    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw_errno("connect");

    if (!write_all(fd.get(), msg.data(), msg.size())) throw std::runtime_error("send");

    // ⚠️ THE #1 BEGINNER TCP BUG: assuming one send() == one recv().
    // TCP IS A BYTE STREAM, NOT A MESSAGE STREAM. Your 100-byte write may
    // arrive as 40 + 60, or be merged with the next one. You MUST define
    // message boundaries yourself: a length prefix, a delimiter (HTTP's
    // \r\n\r\n), or a fixed size. Here we know we expect msg.size() bytes.
    std::string out;
    char buf[4096];
    while (out.size() < msg.size()) {
        ssize_t n = ::recv(fd.get(), buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

// ===========================================================================
int main(int argc, char** argv) {
    // Mode 1: act as a real server so you can `nc localhost 9000`.
    if (argc >= 2 && std::string(argv[1]) == "serve") {
        std::uint16_t port = static_cast<std::uint16_t>(argc >= 3 ? std::atoi(argv[2]) : 9000);
        std::atomic<bool> stop{false};
        std::atomic<int> active{0}, total{0};
        std::cout << "serving on " << port << " — try: nc localhost " << port << "\n";
        run_server(port, stop, active, total);
        return 0;
    }

    // Mode 2 (default): self-test.
    std::cout << "\n=== Blocking, thread-per-connection TCP server ===\n";
    constexpr std::uint16_t PORT = 9101;

    std::atomic<bool> stop{false};
    std::atomic<int>  active{0}, total{0};
    std::thread server([&]{
        try { run_server(PORT, stop, active, total); }
        catch (const std::exception& e) { std::cerr << "  [server] " << e.what() << "\n"; }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // let it bind

    // Drive it with a handful of concurrent clients.
    constexpr int CLIENTS = 20;
    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < CLIENTS; ++i) {
        clients.emplace_back([&, i]{
            try {
                std::string msg = "hello-from-client-" + std::to_string(i);
                std::string resp = client_roundtrip(PORT, msg);
                std::string expected = msg;
                for (char& c : expected) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (resp == expected) ++ok;
                else std::cerr << "  mismatch: '" << resp << "'\n";
            } catch (const std::exception& e) {
                std::cerr << "  [client " << i << "] " << e.what() << "\n";
            }
        });
    }
    for (auto& c : clients) c.join();
    auto ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0).count();

    std::cout << "  " << ok.load() << "/" << CLIENTS
              << " clients round-tripped correctly in " << ms << " ms\n";
    std::cout << "  server accepted " << total.load() << " connections, "
              << "each on its own thread\n";

    stop = true;
    // Kick the blocking accept() by connecting once more, so the loop can exit.
    try { (void)client_roundtrip(PORT, "x"); } catch (...) {}
    server.join();

    std::cout <<
      "\n=== WHY THIS ARCHITECTURE HITS A WALL ===\n"
      "  Per connection you pay:\n"
      "    - 512 KB of stack (macOS) or 8 MB (Linux default) of VIRTUAL memory\n"
      "    - a kernel thread: scheduler entry, TLS block, kernel stack\n"
      "    - a context switch every time the connection blocks on I/O\n"
      "      (~1-3 µs each, PLUS a cold cache afterwards, which is worse)\n"
      "\n"
      "  10,000 connections = 10,000 threads = 5 GB of stack address space and a\n"
      "  scheduler run queue the kernel spends real time managing. Throughput\n"
      "  collapses long before you run out of memory, because the machine is\n"
      "  spending its cycles switching rather than working.\n"
      "\n"
      "  This is 'the C10K problem' (Dan Kegel, 1999) — and it's why every\n"
      "  modern server is EVENT DRIVEN instead: a handful of threads, each\n"
      "  multiplexing thousands of sockets with epoll/kqueue.\n"
      "\n"
      "  Java's answer was NIO + Netty. Node's was libuv. Go's was goroutines\n"
      "  (userspace-scheduled stacks, which is a third approach). C++'s answer\n"
      "  is what you'll build next: 08-systems/03-kqueue-event-loop.cpp\n"
      "\n"
      "  ⚠️ IN FAIRNESS: thread-per-connection is FINE and often correct for\n"
      "     hundreds of connections doing heavy CPU work per request. Don't\n"
      "     build an event loop you don't need. Know both, choose deliberately.\n";
    return 0;
}
