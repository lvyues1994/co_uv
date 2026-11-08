#include <uvco/uvco.hpp>
#include "latency_stats.hpp"
#include <atomic>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cstring>
// Counts C++ allocation calls, NOT malloc/libuv/kernel allocations. Enabled only
// during measurement; no backtrace/container allocations inside the hook.
static std::atomic<unsigned long long> allocations{0};
static bool counting = false, track = true;
void *operator new(std::size_t n) {
    if (counting && track)
        ++allocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) {
    return ::operator new(n);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}
using namespace uvco;
template <class T> void need(T const &r) {
    if (!r)
        throw std::runtime_error("I/O failed");
}
struct Run {
    std::size_t rounds = 10000, warmup = 100, bytes = 128;
    std::vector<double> samples;
    std::uint64_t begin = 0, end = 0;
    Run(std::size_t r, std::size_t b) : rounds(r), bytes(b) {
        samples.reserve(r);
    }
};
void report(char const *name, Run const &r, unsigned long long n) {
    auto s = uvco_bench::summarize(r.samples);
    double elapsed = (r.end - r.begin) / 1e9;
    std::printf("%s bytes=%zu n=%zu p50_us=%.3f p99_us=%.3f max_us=%.3f echo_MiB_s=%.2f "
                "new_total=%llu new_per_round=%.4f\n",
                name, r.bytes, r.samples.size(), s.p50, s.p99, s.max,
                r.rounds * r.bytes / 1048576.0 / elapsed, n, double(n) / r.rounds);
}
auto tcpServer(Tcp &listener, Run &r)
    CO2_BEG(co2::Task<>,(listener,r),Result<Tcp> peer{Errc::canceled};std::size_t i=0,offset=0;std::vector<char> data;Result<std::size_t> n{Errc::canceled};Result<void> sent;) {
    CO2_AWAIT_SET(peer, listener.accept());
    need(peer);
    need(peer->setNoDelay(true));
    data.resize(r.bytes);
    for (i = 0; i < r.rounds + r.warmup; ++i) {
        offset = 0;
        while (offset < r.bytes) {
            CO2_AWAIT_SET(n, peer->read({data.data() + offset, r.bytes - offset}));
            need(n);
            if (*n == 0)
                throw std::runtime_error("zero TCP read");
            offset += *n;
        }
        CO2_AWAIT_SET(sent, peer->write(buffer(data)));
        need(sent);
    }
    CO2_AWAIT(peer->close());
    CO2_RETURN();
}
CO2_END
auto tcpClient(Tcp &client, SocketAddress address, Run &r)
    CO2_BEG(co2::Task<>,(client,address,r),std::size_t i=0,offset=0;std::uint64_t start=0;std::vector<char> data,echo;Result<void> sent;Result<std::size_t> n{Errc::canceled};) {
    CO2_AWAIT_SET(sent, client.connect(address));
    need(sent);
    need(client.setNoDelay(true));
    data.assign(r.bytes, 'x');
    echo.resize(r.bytes);
    for (i = 0; i < r.rounds + r.warmup; ++i) {
        if (i == r.warmup) {
            allocations = 0;
            r.begin = uv_hrtime();
        }
        start = uv_hrtime();
        CO2_AWAIT_SET(sent, client.write(buffer(data)));
        need(sent);
        offset = 0;
        while (offset < r.bytes) {
            CO2_AWAIT_SET(n, client.read({echo.data() + offset, r.bytes - offset}));
            need(n);
            if (*n == 0)
                throw std::runtime_error("zero TCP read");
            offset += *n;
        }
        if (echo != data)
            throw std::runtime_error("TCP echo mismatch");
        if (i >= r.warmup)
            r.samples.push_back(static_cast<double>(uv_hrtime() - start) / 1000.0);
    }
    r.end = uv_hrtime();
    counting = false;
    CO2_AWAIT(client.close());
    CO2_RETURN();
}
CO2_END
auto tcpPair(Tcp &client, Tcp &listener, SocketAddress address, Run &r)
    CO2_BEG(co2::Task<>,(client,listener,address,r)) {
    CO2_AWAIT(co2::whenAll(tcpClient(client, address, r), tcpServer(listener, r)));
    CO2_RETURN();
}
CO2_END
template <class H> auto closeH(H &h) CO2_BEG(co2::Task<>,(h)) {
    CO2_AWAIT(h.close());
    CO2_RETURN();
}
CO2_END

struct Raw {
    uv_loop_t loop{};
    uv_tcp_t listener{}, client{}, peer{};
    uv_connect_t connect{};
    uv_write_t cw{}, sw{};
    Run &r;
    std::vector<char> tx, rx, server;
    std::size_t ci = 0, si = 0, i = 0;
    std::uint64_t start = 0;
    bool clientWriting = false, clientReceived = false;
    explicit Raw(Run &run) : r(run), tx(r.bytes, 'x'), rx(r.bytes), server(r.bytes) {}
    static Raw &self(uv_handle_t *h) {
        return *static_cast<Raw *>(h->data);
    }
    static void check(int rc) {
        if (rc < 0) {
            std::fprintf(stderr, "raw libuv: %s\n", uv_strerror(rc));
            std::abort();
        }
    }
    static void alloc(uv_handle_t *h, std::size_t, uv_buf_t *b) {
        auto &s = self(h);
        bool c = h == (uv_handle_t *)&s.client;
        auto off = c ? s.ci : s.si;
        *b = uv_buf_init((c ? s.rx.data() : s.server.data()) + off, unsigned(s.r.bytes - off));
    }
    // Same fast path as uvco, including uv_write fallback and stable borrowed storage.
    bool write(uv_tcp_t &h, std::vector<char> &data, uv_write_t &req, uv_write_cb cb) {
        uv_buf_t b = uv_buf_init(data.data(), unsigned(data.size()));
        int n = uv_try_write((uv_stream_t *)&h, &b, 1);
        if (n == int(data.size()))
            return true;
        if (n < 0 && n != UV_EAGAIN)
            check(n);
        if (n > 0) {
            b.base += n;
            b.len -= n;
        }
        req.data = this;
        check(uv_write(&req, (uv_stream_t *)&h, &b, 1, cb));
        return false;
    }
    void send() {
        if (i == r.warmup) {
            allocations = 0;
            r.begin = uv_hrtime();
        }
        start = uv_hrtime();
        clientReceived = false;
        clientWriting = !write(client, tx, cw, [](uv_write_t *req, int rc) {
            check(rc);
            auto &s = *static_cast<Raw *>(req->data);
            s.clientWriting = false;
            if (s.clientReceived)
                s.next();
        });
        ci = 0;
        check(uv_read_start((uv_stream_t *)&client, alloc, read));
    }
    static void read(uv_stream_t *h, ssize_t n, uv_buf_t const *) {
        auto &s = self((uv_handle_t *)h);
        if (n == 0)
            return;
        if (n < 0)
            check(int(n));
        bool c = h == (uv_stream_t *)&s.client;
        auto &off = c ? s.ci : s.si;
        off += std::size_t(n);
        if (off < s.r.bytes)
            return;
        check(uv_read_stop(h));
        if (c) {
            if (s.rx != s.tx)
                std::abort();
            if (s.i >= s.r.warmup)
                s.r.samples.push_back(double(uv_hrtime() - s.start) / 1000);
            s.clientReceived = true;
            if (!s.clientWriting)
                s.next();
        } else {
            s.si = 0;
            if (s.write(s.peer, s.server, s.sw, [](uv_write_t *req, int rc) {
                    check(rc);
                    auto &s = *static_cast<Raw *>(req->data);
                    check(uv_read_start((uv_stream_t *)&s.peer, alloc, read));
                }))
                check(uv_read_start((uv_stream_t *)&s.peer, alloc, read));
        }
    }
    void next() {
        ++i;
        if (i == r.rounds + r.warmup) {
            r.end = uv_hrtime();
            counting = false;
            uv_close((uv_handle_t *)&client, nullptr);
            uv_close((uv_handle_t *)&peer, nullptr);
            uv_close((uv_handle_t *)&listener, nullptr);
        } else
            send();
    }
    void run() {
        check(uv_loop_init(&loop));
        check(uv_tcp_init(&loop, &listener));
        check(uv_tcp_init(&loop, &client));
        listener.data = client.data = this;
        connect.data = this;
        sockaddr_in a{};
        check(uv_ip4_addr("127.0.0.1", 0, &a));
        check(uv_tcp_bind(&listener, (sockaddr *)&a, 0));
        check(uv_listen((uv_stream_t *)&listener, 128, [](uv_stream_t *h, int rc) {
            check(rc);
            auto &s = self((uv_handle_t *)h);
            check(uv_tcp_init(&s.loop, &s.peer));
            s.peer.data = &s;
            check(uv_accept(h, (uv_stream_t *)&s.peer));
            check(uv_tcp_nodelay(&s.peer, 1));
            check(uv_read_start((uv_stream_t *)&s.peer, alloc, read));
        }));
        int len = sizeof(a);
        check(uv_tcp_getsockname(&listener, (sockaddr *)&a, &len));
        check(uv_tcp_connect(&connect, &client, (sockaddr *)&a, [](uv_connect_t *req, int rc) {
            check(rc);
            auto &s = *static_cast<Raw *>(req->data);
            check(uv_tcp_nodelay(&s.client, 1));
            s.send();
        }));
        uv_run(&loop, UV_RUN_DEFAULT);
        check(uv_loop_close(&loop));
    }
};
auto sleeps(Loop &l, Run &r)
    CO2_BEG(co2::Task<>,(l,r),std::size_t i=0;std::uint64_t start=0;Result<void> res;) {
    for (i = 0; i < r.rounds + r.warmup; ++i) {
        if (i == r.warmup) {
            allocations = 0;
            r.begin = uv_hrtime();
        }
        start = uv_hrtime();
        CO2_AWAIT_SET(res, sleep(l, std::chrono::milliseconds{0}));
        need(res);
        if (i >= r.warmup)
            r.samples.push_back(double(uv_hrtime() - start) / 1000);
    }
    r.end = uv_hrtime();
    counting = false;
    CO2_RETURN();
}
CO2_END
auto sleepsLocal(Loop &l, Run &r)
    CO2_BEG(co2::Task<>,(l,r),std::size_t i=0;std::uint64_t start=0;Result<void> res;Sleep nap{l,std::chrono::milliseconds{0}};) {
    for (i = 0; i < r.rounds + r.warmup; ++i) {
        if (i == r.warmup) {
            allocations = 0;
            r.begin = uv_hrtime();
        }
        start = uv_hrtime();
        CO2_AWAIT_SET(res, nap);
        need(res);
        if (i >= r.warmup)
            r.samples.push_back(double(uv_hrtime() - start) / 1000);
    }
    r.end = uv_hrtime();
    counting = false;
    CO2_RETURN();
}
CO2_END
auto ticks(Loop &l, Run &r) CO2_BEG(co2::Task<>,(l,r),std::size_t i=0;std::uint64_t start=0;Result<std::uint64_t> res;Timer timer{l};) {
    for (i = 0; i < r.rounds + r.warmup; ++i) {
        if (i == r.warmup) {
            allocations = 0;
            r.begin = uv_hrtime();
        }
        start = uv_hrtime();
        need(timer.start(std::chrono::milliseconds{0}));
        CO2_AWAIT_SET(res, timer.tick());
        need(res);
        if (i >= r.warmup)
            r.samples.push_back(double(uv_hrtime() - start) / 1000);
    }
    r.end = uv_hrtime();
    counting = false;
    CO2_AWAIT(timer.close());
    CO2_RETURN();
}
CO2_END
struct RawTimer {
    uv_loop_t loop{};
    uv_timer_t timer{};
    Run &r;
    std::size_t i = 0;
    std::uint64_t start = 0;
    bool closeEach;
    RawTimer(Run &run, bool close) : r(run), closeEach(close) {}
    void arm() {
        if (i == r.warmup) {
            allocations = 0;
            r.begin = uv_hrtime();
        }
        start = uv_hrtime();
        if (closeEach)
            Raw::check(uv_timer_init(&loop, &timer));
        timer.data = this;
        Raw::check(uv_timer_start(
            &timer,
            [](uv_timer_t *t) {
                auto &s = *static_cast<RawTimer *>(t->data);
                if (s.closeEach)
                    uv_close((uv_handle_t *)t,
                             [](uv_handle_t *h) { static_cast<RawTimer *>(h->data)->done(); });
                else
                    s.done();
            },
            0, 0));
    }
    void done() {
        if (i >= r.warmup)
            r.samples.push_back(double(uv_hrtime() - start) / 1000);
        if (++i < r.rounds + r.warmup)
            arm();
        else {
            r.end = uv_hrtime();
            counting = false;
            if (!closeEach)
                uv_close((uv_handle_t *)&timer, nullptr);
        }
    }
    void run() {
        Raw::check(uv_loop_init(&loop));
        if (!closeEach)
            Raw::check(uv_timer_init(&loop, &timer));
        arm();
        uv_run(&loop, UV_RUN_DEFAULT);
        Raw::check(uv_loop_close(&loop));
    }
};
auto emptyTask() CO2_BEG(co2::Task<>,()) {
    CO2_RETURN();
}
CO2_END
int main(int argc, char **argv) {
    try {
        for (int a = 1; a < argc; ++a)
            if (std::strcmp(argv[a], "no-count") == 0)
                track = false;
        std::printf("allocation_hook=%s\n", track ? "ON" : "OFF");
        std::printf("sizeof read=%zu write=%zu sleep=%zu udp_send=%zu co2_inline=%zu\n",
                    sizeof(decltype(std::declval<Tcp &>().read(MutableBuffer{}))),
                    sizeof(decltype(std::declval<Tcp &>().write(ConstBuffer{}))), sizeof(Sleep),
                    sizeof(decltype(std::declval<Udp &>().send(ConstBuffer{}))),
                    std::size_t(CO2_AWAIT_STORAGE_SIZE));
        bool reverse = argc > 1 && std::strcmp(argv[1], "reverse") == 0;
        for (auto bytes : {std::size_t(128), std::size_t(65536)})
            for (int j = 0; j < 2; ++j) {
                bool raw = (j == 0) != reverse;
                Run r{bytes == 128 ? 10000u : 2000u, bytes};
                allocations = 0;
                counting = true;
                if (raw) {
                    Raw x{r};
                    x.run();
                } else {
                    Loop l;
                    Tcp listener{l}, client{l};
                    need(listener.bind(*SocketAddress::ipv4("127.0.0.1", 0)));
                    need(listener.listen());
                    run(l, tcpPair(client, listener, *listener.localAddress(), r));
                    run(l, closeH(listener));
                    drain(l);
                }
                report(raw ? "raw_tcp" : "uvco_tcp", r, allocations.load());
            }
        for (int mode = 0; mode < 5; ++mode) {
            Run r{20000, 1};
            allocations = 0;
            counting = true;
            if (mode >= 2) {
                Loop l;
                if (mode == 2)
                    run(l, sleeps(l, r));
                else if (mode == 3)
                    run(l, sleepsLocal(l, r));
                else
                    run(l, ticks(l, r));
                drain(l);
            } else {
                RawTimer t{r, mode == 1};
                t.run();
            }
            report(mode == 0   ? "raw_timer_reuse"
                   : mode == 1 ? "raw_timer_close"
                   : mode == 2 ? "uvco_sleep_rvalue"
                   : mode == 3 ? "uvco_sleep_local"
                               : "uvco_timer_tick",
                   r, allocations.load());
        }
        allocations = 0;
        counting = true;
        for (int i = 0; i < 10000; ++i) {
            auto t = emptyTask();
        }
        counting = false;
        std::printf("task_create new_per_task=%.4f\n", allocations.load() / 10000.0);
        return 0;
    } catch (std::exception const &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
