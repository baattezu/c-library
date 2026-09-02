// ===========================================================================
//  AN EVENT LOOP — one thread, thousands of connections
//  run: make run F=08-systems/03-kqueue-event-loop.cpp
//  serve: ./build/08-systems/03-kqueue-event-loop serve 9000
// ===========================================================================
//
//  THIS FILE IS THE POINT OF THE WHOLE SYSTEMS SECTION.
//
//  Thread-per-connection fails at ~10k connections because a thread costs a
//  stack and a context switch. The fix, invented in the late 90s and now
//  universal, is READINESS-BASED MULTIPLEXING:
//
//     - set every socket to NON-BLOCKING
//     - ask the kernel ONE question: "which of these 10,000 fds are ready?"
//     - handle exactly those, without ever blocking
//     - repeat
//
//  ONE syscall serves ten thousand sockets. One thread. No context switches.
//  Memory per connection drops from 512 KB of stack to ~10 KB of buffers.
//
//  THE APIS, ACROSS PLATFORMS
//    select()   POSIX. O(n) scan, hard 1024-fd limit. Obsolete.
//    poll()     POSIX. O(n) scan, no limit. Portable fallback.
//    epoll      Linux. O(1). What nginx/Netty/Node use in production.
//    kqueue     BSD/macOS. O(1). Same idea, arguably a nicer API.  ← THIS FILE
//    IOCP       Windows. Completion-based rather than readiness-based.
//    io_uring   Linux 5.1+. Completion-based, shared-memory rings, batched.
//               The current state of the art.
//
//  We use kqueue because you're on macOS. Every concept maps 1:1 to epoll;
//  the epoll equivalent is noted at each step.
//
// ===========================================================================

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/event.h>       // kqueue, kevent   (Linux: <sys/epoll.h>)
#include <sys/socket.h>
#include <unistd.h>

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

// ---------------------------------------------------------------------------
// ⭐ NON-BLOCKING MODE IS THE WHOLE FOUNDATION.
// With O_NONBLOCK set, read/write/accept NEVER block. If there's nothing to do
// they return -1 with errno == EAGAIN (== EWOULDBLOCK), which means
// "not an error — come back when the kernel says I'm ready."
// ---------------------------------------------------------------------------
static void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) throw_errno("fcntl F_GETFL");
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) throw_errno("fcntl F_SETFL");
}

// ===========================================================================
//  PER-CONNECTION STATE
//
//  This is the key structural difference from the blocking model. There, the
//  connection's state lived implicitly in a THREAD'S STACK (the local variables
//  and the program counter). Here there is no per-connection stack, so you must
//  make the state EXPLICIT: a struct plus, in a real protocol, a state machine.
//
//  This is why callback-based async code is harder to write than blocking code,
//  and it's exactly the problem coroutines (and Go's goroutines, and Java's
//  virtual threads) exist to solve: they give you back the implicit stack.
// ===========================================================================
struct Connection {
    Fd          fd;
    std::string inbuf;              // bytes received, not yet processed
    std::string outbuf;             // bytes to send, not yet accepted by the kernel
    bool        want_write = false; // are we currently registered for writability?
    bool        closing    = false;
    std::size_t bytes_in = 0, bytes_out = 0;
};

// ===========================================================================
//  THE EVENT LOOP
// ===========================================================================
class EventLoop {
    Fd  kq_;                                            // Linux: the epoll fd
    Fd  listener_;
    std::unordered_map<int, Connection> conns_;         // fd -> state
    std::atomic<bool>* stop_ = nullptr;

    // Register/unregister interest. kqueue uses a "changelist" of kevent
    // structs; epoll uses epoll_ctl(ADD/MOD/DEL) with an epoll_event.
    void watch(int fd, int16_t filter, uint16_t flags) {
        struct kevent ev;
        //           ident, filter,  flags, fflags, data, udata
        EV_SET(&ev, fd, filter, flags, 0, 0, nullptr);
        if (::kevent(kq_.get(), &ev, 1, nullptr, 0, nullptr) < 0 && errno != ENOENT)
            throw_errno("kevent register");
    }

public:
    std::atomic<int> total_conns{0}, total_bytes{0};

    EventLoop(std::uint16_t port, std::atomic<bool>* stop) : stop_(stop) {
        // 1. Create the kqueue.  (Linux: epoll_create1(EPOLL_CLOEXEC))
        kq_ = Fd{::kqueue()};
        if (!kq_) throw_errno("kqueue");

        // 2. Create, configure and bind the listening socket.
        listener_ = Fd{::socket(AF_INET, SOCK_STREAM, 0)};
        if (!listener_) throw_errno("socket");
        int yes = 1;
        ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(port);
        if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw_errno("bind");
        if (::listen(listener_.get(), 512) < 0) throw_errno("listen");

        // 3. THE LISTENER IS JUST ANOTHER FD. "Readable" on a listening socket
        //    means "a connection is waiting in the accept queue."
        set_nonblocking(listener_.get());
        watch(listener_.get(), EVFILT_READ, EV_ADD | EV_ENABLE);
    }

    void run() {
        constexpr int MAX_EVENTS = 256;
        struct kevent events[MAX_EVENTS];

        while (!stop_->load(std::memory_order_relaxed)) {
            // ⭐ THE ONE SYSCALL. It blocks until at least one fd is ready (or
            // the timeout expires) and returns ONLY the ready ones.
            //
            // O(number of READY fds), not O(number of WATCHED fds). That is the
            // entire difference from select/poll, and the entire reason this
            // scales to 100k connections.
            //
            // Linux equivalent: epoll_wait(epfd, events, MAX_EVENTS, timeout_ms)
            struct timespec timeout{0, 50 * 1000 * 1000};   // 50 ms
            int n = ::kevent(kq_.get(), nullptr, 0, events, MAX_EVENTS, &timeout);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw_errno("kevent wait");
            }

            for (int i = 0; i < n; ++i) {
                int fd = static_cast<int>(events[i].ident);

                // EV_EOF means the peer half-closed. (epoll: EPOLLHUP/EPOLLRDHUP)
                if (events[i].flags & EV_ERROR) { close_conn(fd); continue; }

                if (fd == listener_.get())        on_accept();
                else if (events[i].filter == EVFILT_READ)  on_readable(fd);
                else if (events[i].filter == EVFILT_WRITE) on_writable(fd);
            }
        }
    }

private:
    // -----------------------------------------------------------------------
    void on_accept() {
        // ⚠️ ACCEPT IN A LOOP UNTIL EAGAIN. Several connections may have arrived
        // since the last wakeup, and with edge-triggered notification you'd
        // never be told about the ones you didn't drain.
        while (true) {
            sockaddr_in peer{};
            socklen_t   len = sizeof(peer);
            int c = ::accept(listener_.get(), reinterpret_cast<sockaddr*>(&peer), &len);
            if (c < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained
                if (errno == EINTR) continue;
                if (errno == ECONNABORTED) continue;    // client vanished; normal
                break;
            }
            set_nonblocking(c);
            int yes = 1;
            ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

            Connection conn;
            conn.fd = Fd{c};
            conns_.emplace(c, std::move(conn));
            watch(c, EVFILT_READ, EV_ADD | EV_ENABLE);
            ++total_conns;
        }
    }

    // -----------------------------------------------------------------------
    void on_readable(int fd) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        Connection& c = it->second;

        char buf[16 * 1024];
        while (true) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                c.inbuf.append(buf, static_cast<std::size_t>(n));
                c.bytes_in += static_cast<std::size_t>(n);
                total_bytes += static_cast<int>(n);
                continue;                              // keep draining
            }
            if (n == 0) { close_conn(fd); return; }    // peer sent FIN
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // drained: normal
            close_conn(fd); return;                    // real error
        }

        // ---- APPLICATION LOGIC: frame the byte stream into messages ----
        // TCP gives you BYTES, not messages. Here the protocol is
        // "newline-delimited". A real HTTP server looks for \r\n\r\n; a binary
        // protocol reads a length prefix. THIS IS ALWAYS YOUR JOB.
        std::size_t pos;
        while ((pos = c.inbuf.find('\n')) != std::string::npos) {
            std::string line = c.inbuf.substr(0, pos);
            c.inbuf.erase(0, pos + 1);
            for (char& ch : line) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            c.outbuf += line;
            c.outbuf += '\n';
        }

        // ⚠️ BACKPRESSURE: if a client sends without ever reading, inbuf/outbuf
        // grow forever and you OOM. A real server caps them and disconnects or
        // stops reading. This is the async equivalent of a bounded queue.
        if (c.inbuf.size() > 1 << 20) { close_conn(fd); return; }

        flush(fd, c);
    }

    // -----------------------------------------------------------------------
    // Try to send outbuf. If the kernel's socket buffer is full, we get EAGAIN;
    // then we must ask to be told when the socket becomes WRITABLE and try
    // again later. Forgetting this half is the classic async-server bug: it
    // "works" on localhost (where the buffer never fills) and stalls in prod.
    void flush(int fd, Connection& c) {
        while (!c.outbuf.empty()) {
            ssize_t n = ::send(fd, c.outbuf.data(), c.outbuf.size(), 0);
            if (n > 0) {
                c.outbuf.erase(0, static_cast<std::size_t>(n));
                c.bytes_out += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!c.want_write) {                     // register for writability
                    watch(fd, EVFILT_WRITE, EV_ADD | EV_ENABLE);
                    c.want_write = true;
                }
                return;
            }
            close_conn(fd); return;                      // EPIPE etc.
        }
        if (c.want_write) {                              // nothing left to send:
            watch(fd, EVFILT_WRITE, EV_DELETE);          // stop being told about
            c.want_write = false;                        // writability (it's almost
        }                                                // always writable!)
    }

    void on_writable(int fd) {
        auto it = conns_.find(fd);
        if (it != conns_.end()) flush(fd, it->second);
    }

    void close_conn(int fd) {
        // kqueue/epoll automatically remove an fd when it is closed, so an
        // explicit EV_DELETE isn't required — but being explicit is clearer and
        // avoids surprises if the fd is duplicated.
        conns_.erase(fd);            // Connection's Fd destructor calls close()
    }
};

// ===========================================================================
//  A CLIENT for the self-test
// ===========================================================================
static std::string client_roundtrip(std::uint16_t port, const std::string& line) {
    Fd fd{::socket(AF_INET, SOCK_STREAM, 0)};
    if (!fd) throw_errno("socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw_errno("connect");

    std::string msg = line + "\n";
    std::size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = ::send(fd.get(), msg.data() + sent, msg.size() - sent, 0);
        if (n <= 0) { if (errno == EINTR) continue; throw_errno("send"); }
        sent += static_cast<std::size_t>(n);
    }

    std::string out;
    char buf[4096];
    while (out.find('\n') == std::string::npos) {
        ssize_t n = ::recv(fd.get(), buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// ===========================================================================
int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "serve") {
        std::uint16_t port = static_cast<std::uint16_t>(argc >= 3 ? std::atoi(argv[2]) : 9000);
        std::atomic<bool> stop{false};
        EventLoop loop(port, &stop);
        std::cout << "event loop serving on " << port
                  << " (line-based uppercase echo). try: nc localhost " << port << "\n";
        loop.run();
        return 0;
    }

    std::cout << "\n=== Single-threaded kqueue event loop ===\n";
    constexpr std::uint16_t PORT = 9202;
    constexpr int CLIENTS = 200;          // 200 concurrent connections, ONE server thread

    std::atomic<bool> stop{false};
    std::atomic<int>  ok{0};

    EventLoop loop(PORT, &stop);
    std::thread server([&]{
        try { loop.run(); }
        catch (const std::exception& e) { std::cerr << "  [loop] " << e.what() << "\n"; }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> clients;
    clients.reserve(CLIENTS);
    for (int i = 0; i < CLIENTS; ++i) {
        clients.emplace_back([&, i]{
            try {
                std::string msg  = "connection-" + std::to_string(i);
                std::string resp = client_roundtrip(PORT, msg);
                std::string want = msg;
                for (char& c : want) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (resp == want) ++ok;
                else std::cerr << "  mismatch: got '" << resp << "' want '" << want << "'\n";
            } catch (const std::exception& e) {
                std::cerr << "  [client " << i << "] " << e.what() << "\n";
            }
        });
    }
    for (auto& c : clients) c.join();
    auto ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0).count();

    stop = true;
    server.join();

    std::cout << "  " << ok.load() << "/" << CLIENTS
              << " connections served correctly in " << ms << " ms\n";
    std::cout << "  the server used ONE thread and accepted "
              << loop.total_conns.load() << " connections, "
              << loop.total_bytes.load() << " bytes\n";

    std::cout <<
      "\n=== WHAT YOU JUST BUILT ===\n"
      "  This is, structurally, what nginx / Node.js / Netty / Redis all are.\n"
      "  ~250 lines. The pieces:\n"
      "    1. non-blocking sockets (EAGAIN means 'not now', not 'error')\n"
      "    2. one readiness syscall for all of them (kqueue/epoll)\n"
      "    3. EXPLICIT per-connection state, because there's no per-connection\n"
      "       stack to hold it implicitly\n"
      "    4. buffered reads AND buffered writes, with writability registration\n"
      "       for when the kernel's send buffer fills\n"
      "    5. framing: turning a byte stream into messages, yourself\n"
      "    6. backpressure limits, so a slow reader can't OOM you\n"
      "\n=== WHAT'S MISSING FOR PRODUCTION ===\n"
      "  - TIMEOUTS. An idle connection must be reaped. (EVFILT_TIMER, or a\n"
      "    timing wheel — the standard structure for O(1) timer management.)\n"
      "  - MULTIPLE LOOPS: one per core, each with its OWN listener using\n"
      "    SO_REUSEPORT so the kernel load-balances accepts. That's the\n"
      "    thread-per-core architecture, and it's how you use 12 cores without\n"
      "    a single shared lock.\n"
      "  - EDGE vs LEVEL TRIGGERED (EV_CLEAR / EPOLLET): edge-triggered notifies\n"
      "    only on CHANGE, so it's fewer syscalls but you MUST drain to EAGAIN\n"
      "    or you hang forever. We used level-triggered here (safer).\n"
      "  - Offloading CPU-heavy work to a thread pool, so one slow request can't\n"
      "    stall the loop. ⚠️ NEVER BLOCK THE EVENT LOOP. One blocking call\n"
      "    (a DNS lookup, a file read, a mutex) stalls ALL connections. This is\n"
      "    the same rule as Node.js and Netty.\n"
      "  - TLS, HTTP parsing, graceful shutdown, connection limits, metrics.\n"
      "\n=== THE HONEST TRADE-OFF ===\n"
      "  Event loops give you 10-100x the connections per machine. They cost you\n"
      "  readability: your logic is shredded across callbacks and state machines\n"
      "  ('callback hell'). Every modern runtime tries to give the readability\n"
      "  back — C++20 coroutines, Rust async/await, Go goroutines, Java virtual\n"
      "  threads. They all compile down to exactly what you see here.\n"
      "\n"
      "  Now you know what's underneath all of them.\n";
    return 0;
}
