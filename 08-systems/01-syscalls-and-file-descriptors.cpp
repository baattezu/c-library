// ===========================================================================
//  SYSCALLS AND FILE DESCRIPTORS — the actual boundary of your program
//  run: make run F=08-systems/01-syscalls-and-file-descriptors.cpp
// ===========================================================================
//
//  In Java you call InputStream.read() and something happens. Here you find out
//  what. Below std::cout, below std::fstream, below libc, there is exactly one
//  mechanism for a program to affect the outside world: THE SYSTEM CALL.
//
//  A syscall is a controlled trap into the kernel:
//    - put the syscall number and args in registers
//    - execute `svc #0` (arm64) / `syscall` (x86-64)
//    - the CPU switches to kernel mode and jumps to the kernel's handler
//    - the kernel validates everything, does the work, returns
//
//  COST: ~100ns-1µs on modern hardware (worse since Spectre/Meltdown
//  mitigations added page-table switching). That's 300-3000 wasted instruction
//  slots. THIS is why buffering exists, why io_uring exists, and why
//  high-performance servers obsess over "syscalls per request."
//
// ===========================================================================

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>          // open, O_RDONLY, O_CREAT...
#include <sys/stat.h>       // fstat
#include <sys/uio.h>        // writev
#include <unistd.h>         // read, write, close, lseek, pipe

// ===========================================================================
//  THE MOST IMPORTANT RAII CLASS IN SYSTEMS PROGRAMMING
//  A file descriptor is a small non-negative int indexing a per-process table
//  in the kernel. It is a RESOURCE: leak it and you eventually hit EMFILE
//  ("too many open files"), which is one of the classic production outages.
// ===========================================================================
class Fd {
    int fd_ = -1;
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}

    // Factory that reports errors properly.
    static Fd open(const char* path, int flags, mode_t mode = 0644) {
        int fd = ::open(path, flags, mode);
        if (fd < 0)
            throw std::system_error(errno, std::system_category(),
                                    std::string("open ") + path);
        return Fd{fd};
    }

    ~Fd() {
        if (fd_ >= 0) {
            // ⚠️ close() CAN FAIL (EIO on a network filesystem, EINTR). We
            // cannot throw from a destructor, so we swallow it. If your data
            // integrity depends on close succeeding, call an explicit close()
            // method and check it. This is a real design tension in RAII.
            ::close(fd_);
        }
    }

    Fd(const Fd&)            = delete;   // an fd has exactly ONE owner
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) ::close(fd_); fd_ = std::exchange(o.fd_, -1); }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
};

// ===========================================================================
//  THE THREE RULES OF CALLING read() AND write()
// ===========================================================================
//   1. THEY RETURN HOW MANY BYTES THEY ACTUALLY MOVED. A short read/write is
//      NORMAL, not an error. You must LOOP.
//   2. A return of -1 with errno == EINTR means "a signal interrupted me,
//      just retry." Not an error.
//   3. read() returning 0 means END OF FILE / the peer closed. Not an error.
//
//  Every beginner networking bug is a violation of rule 1.
// ===========================================================================

// Write ALL n bytes, or throw. This function (or its equivalent) exists in
// every serious C++ codebase.
static void write_all(int fd, const void* buf, std::size_t n) {
    const char* p = static_cast<const char*>(buf);
    std::size_t written = 0;
    while (written < n) {
        ssize_t r = ::write(fd, p + written, n - written);
        if (r < 0) {
            if (errno == EINTR) continue;                    // rule 2
            throw std::system_error(errno, std::system_category(), "write");
        }
        written += static_cast<std::size_t>(r);              // rule 1
    }
}

// Read up to n bytes; returns how many. 0 means EOF.
static std::size_t read_some(int fd, void* buf, std::size_t n) {
    while (true) {
        ssize_t r = ::read(fd, buf, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::system_category(), "read");
        }
        return static_cast<std::size_t>(r);                  // may be 0 = EOF
    }
}

// ---------------------------------------------------------------------------
void demo_basic_io() {
    std::cout << "\n=== 1. open / write / read / close ===\n";
    const char* path = "/tmp/cpp_syscall_demo.txt";

    {
        Fd fd = Fd::open(path, O_WRONLY | O_CREAT | O_TRUNC);
        std::cout << "  opened fd=" << fd.get() << "\n";
        const std::string msg = "hello from a raw write(2) syscall\n";
        write_all(fd.get(), msg.data(), msg.size());
        std::cout << "  wrote " << msg.size() << " bytes\n";
    }   // <- close(2) happens HERE, in the destructor. Cannot be forgotten.

    {
        Fd fd = Fd::open(path, O_RDONLY);
        // fstat gives you metadata WITHOUT a second path lookup (and without
        // the TOCTOU race that stat-then-open has).
        struct stat st{};
        if (::fstat(fd.get(), &st) < 0)
            throw std::system_error(errno, std::system_category(), "fstat");
        std::cout << "  file size = " << st.st_size << " bytes\n";

        char buf[256];
        std::size_t n = read_some(fd.get(), buf, sizeof(buf));
        std::cout << "  read " << n << " bytes: " << std::string(buf, n);

        std::size_t eof = read_some(fd.get(), buf, sizeof(buf));
        std::cout << "  second read returned " << eof << " -> that's EOF\n";
    }

    std::cout <<
      "\n  THE STANDARD FILE DESCRIPTORS (always open at process start):\n"
      "    0 = stdin   1 = stdout   2 = stderr\n"
      "  Shell redirection (`2>&1`) is literally dup2(1, 2) — pointing fd 2 at\n"
      "  whatever fd 1 refers to. Now you know what that syntax means.\n";
}

// ---------------------------------------------------------------------------
void demo_syscall_cost() {
    std::cout << "\n=== 2. What a syscall costs, and why buffering exists ===\n";

    const char* path = "/tmp/cpp_syscall_bench.txt";
    constexpr int N = 20000;
    const char byte = 'x';

    // (a) One syscall per byte. This is what unbuffered I/O looks like.
    {
        Fd fd = Fd::open(path, O_WRONLY | O_CREAT | O_TRUNC);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) write_all(fd.get(), &byte, 1);
        auto t1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        std::printf("  %d separate write(2) calls: %8.0f us  (%.2f us/call)\n",
                    N, us, us / N);
    }

    // (b) Buffer in userspace, ONE syscall. This is what FILE*/ofstream do.
    {
        Fd fd = Fd::open(path, O_WRONLY | O_CREAT | O_TRUNC);
        std::vector<char> buffer(N, byte);
        auto t0 = std::chrono::steady_clock::now();
        write_all(fd.get(), buffer.data(), buffer.size());
        auto t1 = std::chrono::steady_clock::now();
        std::printf("  1 buffered write(2) of %d bytes: %8.0f us\n",
                    N, std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    // (c) writev / "scatter-gather": send several non-contiguous buffers in ONE
    //     syscall. This is how you send an HTTP header + body without
    //     concatenating them into a new allocation. Real servers use this.
    {
        Fd fd = Fd::open(path, O_WRONLY | O_CREAT | O_TRUNC);
        std::string header = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
        std::string body   = "hello";
        struct iovec iov[2];
        iov[0].iov_base = header.data(); iov[0].iov_len = header.size();
        iov[1].iov_base = body.data();   iov[1].iov_len = body.size();
        ssize_t w = ::writev(fd.get(), iov, 2);
        std::printf("  writev: sent header+body (%zd bytes) in ONE syscall,\n"
                    "          with no concatenation and no allocation\n", w);
    }
    ::unlink(path);
    ::unlink("/tmp/cpp_syscall_demo.txt");

    std::cout <<
      "\n  ⭐ THE HIGH-LOAD PRINCIPLE: COUNT YOUR SYSCALLS PER REQUEST.\n"
      "     A naive HTTP server does: accept, read, read, write, write, close\n"
      "     = 6+ syscalls x ~500ns = 3µs of pure kernel overhead per request,\n"
      "     before any of your logic runs. At 100k req/s that's 30% of a core.\n"
      "\n"
      "     How real servers cut it:\n"
      "       - buffer reads/writes in userspace (1 syscall per buffer, not per\n"
      "         message)\n"
      "       - writev/sendmsg to combine buffers\n"
      "       - sendfile()/splice() for file->socket with NO userspace copy\n"
      "       - accept4() to set O_NONBLOCK|O_CLOEXEC without extra fcntl calls\n"
      "       - epoll/kqueue: ONE syscall reports readiness for 10,000 sockets\n"
      "       - io_uring (Linux 5.1+): submit and complete BATCHES of operations\n"
      "         through shared memory rings — often ZERO syscalls in steady state.\n"
      "         This is the current state of the art and why it's a big deal.\n"
      "\n"
      "     Tools: `strace -c ./prog` (Linux) or `dtruss ./prog` (macOS, needs\n"
      "     sudo and SIP considerations) count syscalls per run. Do this once on\n"
      "     any server you own — the number is usually shocking.\n";
}

// ---------------------------------------------------------------------------
void demo_errno_and_pipes() {
    std::cout << "\n=== 3. errno, and pipes ===\n";

    // errno is a THREAD-LOCAL int set by failing syscalls. It is only
    // meaningful immediately after a call that reported failure — a successful
    // call does NOT reset it.
    int bad = ::open("/definitely/not/here", O_RDONLY);
    if (bad < 0) {
        std::printf("  open failed: errno=%d (%s)\n", errno, std::strerror(errno));
        std::error_code ec(errno, std::system_category());
        std::printf("  as std::error_code: '%s', is-no-such-file=%d\n",
                    ec.message().c_str(),
                    ec == std::errc::no_such_file_or_directory);
    }

    // A pipe: two fds, one read end and one write end, connected by a kernel
    // buffer (64 KB by default). This is what `a | b` in your shell creates.
    int fds[2];
    if (::pipe(fds) < 0) throw std::system_error(errno, std::system_category(), "pipe");
    Fd rd{fds[0]}, wr{fds[1]};

    const std::string msg = "through a pipe";
    write_all(wr.get(), msg.data(), msg.size());
    char buf[64];
    std::size_t n = read_some(rd.get(), buf, sizeof(buf));
    std::printf("  pipe round trip: '%s'\n", std::string(buf, n).c_str());

    std::cout <<
      "\n  Pipes are the simplest 'file descriptor that isn't a file'. Everything\n"
      "  in Unix is an fd: files, sockets, pipes, terminals, timers (timerfd),\n"
      "  signals (signalfd), events (eventfd), even other processes (pidfd).\n"
      "  That uniformity is what makes ONE event loop able to wait on all of\n"
      "  them at once. It is the single best design idea in Unix.\n";
}

// ---------------------------------------------------------------------------
void the_essentials() {
    std::cout << "\n=== 4. The syscalls worth knowing by name ===\n";
    std::cout <<
      "  FILES\n"
      "    open close read write lseek  pread/pwrite (offset, no shared cursor)\n"
      "    fstat fsync ftruncate  unlink rename  mmap munmap\n"
      "    readv writev           (scatter/gather)\n"
      "    sendfile / copy_file_range  (kernel-side copy, no userspace round trip)\n"
      "\n  PROCESSES\n"
      "    fork execve wait4 _exit  posix_spawn (prefer this)  kill  getpid\n"
      "\n  SOCKETS  (all of 08-systems/02..04)\n"
      "    socket bind listen accept connect  send recv sendto recvfrom\n"
      "    setsockopt getsockopt shutdown  getaddrinfo\n"
      "\n  EVENT NOTIFICATION\n"
      "    select   — O(n), 1024-fd limit, portable, obsolete\n"
      "    poll     — O(n), no limit, portable\n"
      "    epoll    — O(1), Linux. THE production answer on Linux.\n"
      "    kqueue   — O(1), BSD/macOS. Same idea, nicer API.\n"
      "    io_uring — Linux 5.1+. Async submission/completion rings in shared\n"
      "               memory. The current state of the art.\n"
      "\n  MEMORY\n"
      "    mmap munmap mprotect madvise  brk/sbrk (legacy)\n"
      "\n  TIME\n"
      "    clock_gettime  nanosleep\n"
      "    ⚠️ Prefer std::chrono::steady_clock for MEASURING durations (it never\n"
      "       jumps). system_clock is wall time and CAN go backwards (NTP).\n"
      "       Using system_clock for a timeout is a real bug.\n"
      "\n  MANUAL PAGES ARE THE DOCUMENTATION: `man 2 read`, `man 2 kqueue`.\n"
      "  Section 2 = syscalls, section 3 = library functions. Read them. They\n"
      "  are precise, complete, and on your machine right now.\n";
}

int main() {
    try {
        demo_basic_io();
        demo_syscall_cost();
        demo_errno_and_pipes();
        the_essentials();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
