#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

#include <co2/co2.hpp>

#include "test.hpp"
#include "uvco/uvco.hpp"

using namespace std::chrono_literals;
using uvco::Errc;
using uvco::Pipe;
using uvco::Result;
using uvco::SocketAddress;
using uvco::Tcp;

namespace {

struct StopLater {
    StopLater(co2::stop_source& source, std::chrono::milliseconds delay)
        : thread{[&source, delay] {
              std::this_thread::sleep_for(delay);
              source.request_stop();
          }} {}
    ~StopLater() { thread.join(); }
    std::thread thread;
};

SocketAddress loopback(std::uint16_t const port = 0) {
    Result<SocketAddress> a = SocketAddress::parse("127.0.0.1", port);
    CHECK(a);
    return *a;
}

// 绑定 127.0.0.1:0 并监听，返回实际地址。
SocketAddress listenLocal(Tcp& server, int const backlog = 128) {
    CHECK(server.bind(loopback()));
    CHECK(server.listen(backlog));
    Result<SocketAddress> a = server.localAddress();
    CHECK(a);
    CHECK(a->port() != 0);
    return *a;
}

std::string pipePath(char const* const tag) {
    std::string path = std::string{UVCO_TEST_PIPE_DIR} + "/uvco-" + tag + "-" +
                       std::to_string(static_cast<long long>(::getpid())) + ".sock";
    std::remove(path.c_str());
    return path;
}

std::string makeMessage(std::size_t const i) {
    std::string msg((i + 1) * 4099, '\0');
    for (std::size_t k = 0; k < msg.size(); ++k)
        msg[k] = static_cast<char>('a' + (k * 7 + i) % 26);
    return msg;
}

char patternByte(std::size_t const chunk) { return static_cast<char>((chunk * 37 + 11) % 251); }

// ---------------------------------------------------------------- 通用协程

template <class S> auto readOnce(S& s, std::vector<char>& buf)
    CO2_BEG(co2::Task<Result<std::size_t>>, (s, buf), Result<std::size_t> n;) {
    CO2_AWAIT_SET(n, s.read(uvco::buffer(buf)));
    CO2_RETURN(n);
}
CO2_END

template <class S> auto readAll(S& s)
    CO2_BEG(co2::Task<std::string>, (s), std::vector<char> buf; std::string got;
            Result<std::size_t> n;) {
    buf.resize(4096);
    for (;;) {
        CO2_AWAIT_SET(n, s.read(uvco::buffer(buf)));
        if (not n) break;
        got.append(buf.data(), *n);
    }
    CHECK(n.error() == Errc::eof);
    CO2_RETURN(got);
}
CO2_END

template <class S> auto writeOnce(S& s, uvco::ConstBuffer buf)
    CO2_BEG(co2::Task<Result<void>>, (s, buf), Result<void> r;) {
    CO2_AWAIT_SET(r, s.write(buf));
    CO2_RETURN(r);
}
CO2_END

template <class S, class Target> auto connectOnce(S& s, Target target)
    CO2_BEG(co2::Task<Result<void>>, (s, target), Result<void> r;) {
    CO2_AWAIT_SET(r, s.connect(target));
    CO2_RETURN(r);
}
CO2_END

template <class S> auto acceptOnce(S& server)
    CO2_BEG(co2::Task<Result<S>>, (server), Result<S> c;) {
    CO2_AWAIT_SET(c, server.accept());
    CO2_RETURN(std::move(c));
}
CO2_END

// client 连接 server，conn 是服务端接受的那一端。
auto connectPair(Tcp& server, Tcp& client, Tcp& conn)
    CO2_BEG(co2::Task<>, (server, client, conn), Result<void> r; Result<Tcp> a;
            Result<SocketAddress> addr;) {
    addr = server.localAddress();
    CHECK(addr);
    CO2_AWAIT_SET(r, client.connect(*addr));
    CHECK(r);
    CO2_AWAIT_SET(a, server.accept());
    CHECK(a);
    conn = std::move(*a);
}
CO2_END

// ---------------------------------------------------------------- echo

template <class S> auto echoSession(S& s)
    CO2_BEG(co2::Task<>, (s), std::vector<char> buf; Result<std::size_t> n; Result<void> w;) {
    buf.resize(16 * 1024);
    for (;;) {
        CO2_AWAIT_SET(n, s.read(uvco::buffer(buf)));
        if (not n) break;
        CO2_AWAIT_SET(w, s.write(uvco::ConstBuffer{buf.data(), *n}));
        CHECK(w);
    }
    CHECK(n.error() == Errc::eof);
    CO2_AWAIT_SET(w, s.shutdown());
    CHECK(w);
    CO2_AWAIT(s.close());
}
CO2_END

template <class S> auto echoServer(S& server, std::size_t clients)
    CO2_BEG(co2::Task<>, (server, clients), std::vector<S> conns; Result<S> c;
            std::vector<co2::Task<>> sessions; std::size_t i{};) {
    for (i = 0; i < clients; ++i) {
        CO2_AWAIT_SET(c, server.accept());
        CHECK(c);
        conns.push_back(std::move(*c));
    }
    for (auto& conn : conns)
        sessions.push_back(echoSession(conn));
    CO2_AWAIT(co2::whenAll(std::move(sessions)));
    CO2_AWAIT(server.close());
}
CO2_END

template <class S, class Target>
auto echoClient(uvco::Loop& loop, Target target, std::string msg)
    CO2_BEG(co2::Task<std::string>, (loop, target, msg), S s{loop}; std::string got;
            Result<void> r; std::vector<uvco::ConstBuffer> parts; std::size_t third{};) {
    CO2_AWAIT_SET(r, s.connect(target));
    CHECK(r);
    // 分三段的 writev。
    third = msg.size() / 3;
    parts.push_back(uvco::ConstBuffer{msg.data(), third});
    parts.push_back(uvco::ConstBuffer{msg.data() + third, third});
    parts.push_back(uvco::ConstBuffer{msg.data() + 2 * third, msg.size() - 2 * third});
    CO2_AWAIT_SET(r, s.write(parts));
    CHECK(r);
    CO2_AWAIT_SET(r, s.shutdown());
    CHECK(r);
    CO2_AWAIT_SET(got, readAll(s));
    CO2_AWAIT(s.close());
    CO2_RETURN(got);
}
CO2_END

auto collect(std::vector<co2::Task<std::string>> tasks)
    CO2_BEG(co2::Task<std::vector<std::string>>, (tasks), std::vector<std::string> out;) {
    CO2_AWAIT_SET(out, co2::whenAll(std::move(tasks)));
    CO2_RETURN(out);
}
CO2_END

template <class S, class Target>
auto echoScenario(uvco::Loop& loop, S& server, Target target, std::size_t n)
    CO2_BEG(co2::Task<bool>, (loop, server, target, n), std::vector<co2::Task<std::string>> clients;
            std::vector<std::string> msgs; std::tuple<std::vector<std::string>> got;
            std::size_t i{}; bool ok{true};) {
    for (i = 0; i < n; ++i)
        msgs.push_back(makeMessage(i));
    for (i = 0; i < n; ++i)
        clients.push_back(echoClient<S, Target>(loop, target, msgs[i]));
    CO2_AWAIT_SET(got, co2::whenAll(echoServer(server, n), collect(std::move(clients))));
    for (i = 0; i < n; ++i)
        ok = ok && std::get<0>(got)[i] == msgs[i];
    CO2_RETURN(ok);
}
CO2_END

TEST_CASE("Tcp: echo server/client, several concurrent clients, writev") {
    uvco::Loop loop;
    Tcp server{loop};
    SocketAddress const addr = listenLocal(server);
    CHECK(server.setNoDelay(true));
    CHECK(uvco::run(loop, echoScenario(loop, server, addr, 8)));
    CHECK(not server.isOpen());
}

TEST_CASE("Pipe: echo server/client") {
    uvco::Loop loop;
    std::string const path = pipePath("echo");
    Pipe server{loop};
    CHECK(server.bind(path));
    CHECK(server.listen());
    Result<std::string> name = server.localName();
    CHECK(name && *name == path);
    CHECK(uvco::run(loop, echoScenario(loop, server, path, 4)));
    std::remove(path.c_str());
}

// ---------------------------------------------------------------- 地址与选项

auto addressScenario(Tcp& server, Tcp& client, Tcp& conn)
    CO2_BEG(co2::Task<>, (server, client, conn), Result<SocketAddress> a; Result<SocketAddress> b;) {
    CO2_AWAIT(connectPair(server, client, conn));
    a = client.localAddress();
    b = conn.peerAddress();
    CHECK(a && b);
    CHECK(a->toString() == b->toString());
    a = client.peerAddress();
    b = server.localAddress();
    CHECK(a && b && a->port() == b->port() && a->ip() == "127.0.0.1");
    CHECK(client.setNoDelay(true));
    CHECK(client.setKeepAlive(true, std::chrono::seconds{30}));
    CHECK(client.setKeepAlive(false));
}
CO2_END

TEST_CASE("Tcp: local/peer address, socket options; SocketAddress parsing") {
    Result<SocketAddress> v4 = SocketAddress::parse("192.168.1.20", 8080);
    CHECK(v4 && v4->isIpv4() && v4->port() == 8080 && v4->toString() == "192.168.1.20:8080");
    Result<SocketAddress> v6 = SocketAddress::parse("::1", 443);
    CHECK(v6 && v6->isIpv6() && v6->toString() == "[::1]:443");
    CHECK(not SocketAddress::parse("not-an-ip", 1));
    CHECK(not SocketAddress::parse("example.com", 80)); // 不做 DNS
    CHECK(SocketAddress{}.family() == AF_UNSPEC);

    uvco::Loop loop;
    Tcp server{loop};
    Tcp client{loop};
    Tcp conn;
    CHECK(not conn.isOpen());
    listenLocal(server);
    uvco::run(loop, addressScenario(server, client, conn));
}

// ---------------------------------------------------------------- EOF / 半关闭

auto eofScenario(Tcp& server, Tcp& client, Tcp& conn)
    CO2_BEG(co2::Task<>, (server, client, conn), std::vector<char> buf; Result<std::size_t> n;
            Result<void> r; std::string got; std::string const hello{"hello"};
            std::string const bye{"bye"};) {
    CO2_AWAIT(connectPair(server, client, conn));
    CO2_AWAIT_SET(r, client.write(uvco::buffer(hello)));
    CHECK(r);
    CO2_AWAIT_SET(r, client.shutdown());
    CHECK(r);
    CO2_AWAIT_SET(got, readAll(conn));
    CHECK(got == "hello");
    buf.resize(16);
    CO2_AWAIT_SET(n, conn.read(uvco::buffer(buf))); // 粘性 EOF
    CHECK(not n && n.error() == Errc::eof);
    // 半关闭：对端关闭写端后仍可以回写。
    CO2_AWAIT_SET(r, conn.write(uvco::buffer(bye)));
    CHECK(r);
    CO2_AWAIT(conn.close());
    CO2_AWAIT_SET(got, readAll(client));
    CHECK(got == "bye");
}
CO2_END

TEST_CASE("Tcp: EOF after peer shutdown is sticky; half-close still writable") {
    uvco::Loop loop;
    Tcp server{loop};
    Tcp client{loop};
    Tcp conn;
    listenLocal(server);
    uvco::run(loop, eofScenario(server, client, conn));
}

// ---------------------------------------------------------------- 大量数据与反压

auto bulkSender(Tcp& s, std::size_t chunks, std::size_t chunkSize, std::size_t& completed)
    CO2_BEG(co2::Task<>, (s, chunks, chunkSize, completed), std::vector<char> data;
            std::size_t i{}; Result<void> r;) {
    data.resize(chunkSize);
    for (i = 0; i < chunks; ++i) {
        std::fill(data.begin(), data.end(), patternByte(i));
        CO2_AWAIT_SET(r, s.write(uvco::buffer(data)));
        CHECK(r);
        ++completed;
        CHECK(s.writeQueueSize() <= chunkSize);
    }
    CO2_AWAIT_SET(r, s.shutdown());
    CHECK(r);
}
CO2_END

auto slowReceiver(uvco::Loop& loop, Tcp& s, std::size_t chunkSize, std::size_t& senderCompleted,
                  std::size_t& completedAtStart, std::size_t& total)
    CO2_BEG(co2::Task<>, (loop, s, chunkSize, senderCompleted, completedAtStart, total),
            std::vector<char> buf; Result<std::size_t> n; Result<void> r; std::size_t k{};) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 150ms)); // 不读：发送方必须被反压
    CHECK(r);
    completedAtStart = senderCompleted;
    buf.resize(64 * 1024 - 7);
    for (;;) {
        CO2_AWAIT_SET(n, s.read(uvco::buffer(buf)));
        if (not n) break;
        for (k = 0; k < *n; ++k)
            if (buf[k] != patternByte((total + k) / chunkSize)) CHECK(false && "corrupted byte");
        total += *n;
    }
    CHECK(n.error() == Errc::eof);
}
CO2_END

auto bulkScenario(uvco::Loop& loop, Tcp& server, Tcp& client, Tcp& conn, std::size_t chunks,
                  std::size_t chunkSize, std::size_t& completedAtStart, std::size_t& total)
    CO2_BEG(co2::Task<>, (loop, server, client, conn, chunks, chunkSize, completedAtStart, total),
            std::size_t completed{};) {
    CO2_AWAIT(connectPair(server, client, conn));
    CO2_AWAIT(co2::whenAll(bulkSender(client, chunks, chunkSize, completed),
                           slowReceiver(loop, conn, chunkSize, completed, completedAtStart, total)));
    CHECK(completed == chunks);
}
CO2_END

TEST_CASE("Tcp: large transfer with backpressure (pull-mode read)") {
    uvco::Loop loop;
    Tcp server{loop};
    Tcp client{loop};
    Tcp conn;
    listenLocal(server);
    std::size_t const chunks = 48;
    std::size_t const chunkSize = 1024 * 1024;
    std::size_t completedAtStart = 0;
    std::size_t total = 0;
    uvco::run(loop, bulkScenario(loop, server, client, conn, chunks, chunkSize, completedAtStart,
                                 total));
    CHECK(total == chunks * chunkSize);
    // 接收方 150ms 不读：内核缓冲区（至多几 MB）填满后写就挂起。
    CHECK(completedAtStart < chunks);
    std::printf("  backpressure: %zu/%zu chunks written before the reader started\n",
                completedAtStart, chunks);
}

auto concurrentWriter(Tcp& client, std::vector<std::vector<char>>& bufs)
    CO2_BEG(co2::Task<>, (client, bufs), std::vector<co2::Task<Result<void>>> writes;
            std::vector<Result<void>> rs; Result<void> r;) {
    for (auto const& b : bufs)
        writes.push_back(writeOnce(client, uvco::buffer(b)));
    CO2_AWAIT_SET(rs, co2::whenAll(std::move(writes))); // 并发写：按调用顺序排队
    for (auto const& w : rs)
        CHECK(w);
    CO2_AWAIT_SET(r, client.shutdown());
    CHECK(r);
}
CO2_END

auto orderedWrites(Tcp& server, Tcp& client, Tcp& conn, std::vector<std::vector<char>>& bufs)
    CO2_BEG(co2::Task<std::string>, (server, client, conn, bufs),
            std::tuple<std::string> got;) {
    CO2_AWAIT(connectPair(server, client, conn));
    CO2_AWAIT_SET(got, co2::whenAll(concurrentWriter(client, bufs), readAll(conn)));
    CO2_RETURN(std::move(std::get<0>(got)));
}
CO2_END

TEST_CASE("Tcp: concurrent writes on one stream are delivered in call order") {
    uvco::Loop loop;
    Tcp server{loop};
    Tcp client{loop};
    Tcp conn;
    listenLocal(server);
    std::vector<std::vector<char>> bufs;
    std::string expected;
    for (std::size_t i = 0; i < 6; ++i) {
        bufs.emplace_back((i + 1) * 300 * 1024, patternByte(i));
        expected.append(bufs.back().data(), bufs.back().size());
    }
    std::string const got = uvco::run(loop, orderedWrites(server, client, conn, bufs));
    CHECK(got == expected);
}

// ---------------------------------------------------------------- 连接失败

TEST_CASE("Tcp/Pipe: connect refused / missing path") {
    uvco::Loop loop;
    SocketAddress freeAddr;
    {
        Tcp probe{loop};
        freeAddr = listenLocal(probe); // 取得一个空闲端口后关闭
    }
    Tcp tcp{loop};
    Result<void> r = uvco::run(loop, connectOnce(tcp, freeAddr));
    CHECK(not r && r.error() == Errc::connection_refused);

    Pipe pipe{loop};
    r = uvco::run(loop, connectOnce(pipe, pipePath("missing")));
    CHECK(not r && r.error() == Errc::no_such_file);
}

// ---------------------------------------------------------------- 取消

auto readTimeout(uvco::Loop& loop, Tcp& server, Tcp& client, Tcp& conn)
    CO2_BEG(co2::Task<>, (loop, server, client, conn), std::vector<char> buf;
            Result<std::size_t> n; Result<void> r; std::string const x{"x"};) {
    CO2_AWAIT(connectPair(server, client, conn));
    buf.resize(8);
    CO2_AWAIT_SET(n, uvco::timeout(loop, 20ms, readOnce(conn, buf)));
    CHECK(not n && n.error() == Errc::timed_out);
    // 读被取消后流仍然可用，数据没有丢。
    CO2_AWAIT_SET(r, client.write(uvco::buffer(x)));
    CHECK(r);
    CO2_AWAIT_SET(n, uvco::timeout(loop, 2000ms, readOnce(conn, buf)));
    CHECK(n && *n == 1 && buf[0] == 'x');
}
CO2_END

TEST_CASE("Tcp: timeout() around read -> ETIMEDOUT, stream stays usable") {
    uvco::Loop loop;
    Tcp server{loop};
    Tcp client{loop};
    Tcp conn;
    listenLocal(server);
    uvco::run(loop, readTimeout(loop, server, client, conn));
}

TEST_CASE("Tcp: read cancelled from another thread; stop-first even with data ready") {
    uvco::Loop loop;
    Tcp server{loop};
    Tcp client{loop};
    Tcp conn;
    listenLocal(server);
    uvco::run(loop, connectPair(server, client, conn));
    std::vector<char> buf(8);
    for (int i = 0; i < 5; ++i) {
        co2::stop_source source;
        StopLater stopper{source, std::chrono::milliseconds{i * 3}};
        Result<std::size_t> n = uvco::run(loop, readOnce(conn, buf), source.get_token());
        CHECK(not n && n.error() == Errc::canceled);
    }
    std::string const data{"abc"};
    CHECK(uvco::run(loop, writeOnce(client, uvco::buffer(data))));
    co2::stop_source stopped;
    stopped.request_stop();
    Result<std::size_t> n = uvco::run(loop, readOnce(conn, buf), stopped.get_token());
    CHECK(not n && n.error() == Errc::canceled);
    n = uvco::run(loop, readOnce(conn, buf));
    CHECK(n && *n == 3 && std::string(buf.data(), 3) == "abc");
}

TEST_CASE("Tcp: accept cancelled from another thread; accept works afterwards") {
    uvco::Loop loop;
    Tcp server{loop};
    SocketAddress const addr = listenLocal(server);
    for (int i = 0; i < 5; ++i) {
        co2::stop_source source;
        StopLater stopper{source, std::chrono::milliseconds{i * 3}};
        Result<Tcp> c = uvco::run(loop, acceptOnce(server), source.get_token());
        CHECK(not c && c.error() == Errc::canceled);
    }
    Tcp client{loop};
    CHECK(uvco::run(loop, connectOnce(client, addr)));
    Result<Tcp> c = uvco::run(loop, acceptOnce(server));
    CHECK(c && c->isOpen());
    Tcp idle{loop};
    c = uvco::run(loop, acceptOnce(idle)); // 未 listen
    CHECK(not c && c.error() == Errc::invalid_argument);
}

// 监听但从不 accept 且 backlog 很小：队列满后新的 SYN 被丢弃，connect 一直挂起。
auto connectUntilStuck(uvco::Loop& loop, SocketAddress addr, std::vector<Tcp>& clients)
    CO2_BEG(co2::Task<bool>, (loop, addr, clients), Result<void> r; std::size_t i{};
            bool stuck{false}; std::string const x{"x"}; std::vector<char> buf;) {
    clients.reserve(32);
    for (i = 0; i < 32 && not stuck; ++i) {
        clients.emplace_back(loop);
        CO2_AWAIT_SET(r, uvco::timeout(loop, 100ms, connectOnce(clients.back(), addr)));
        if (r) continue;
        CHECK(r.error() == Errc::timed_out);
        stuck = true;
    }
    if (stuck) {
        // connect 被取消 = 流被关闭：之后的操作返回 bad_descriptor。
        CHECK(clients.back().isOpen());
        CO2_AWAIT_SET(r, clients.back().write(uvco::buffer(x)));
        CHECK(not r && r.error() == Errc::bad_descriptor);
        CO2_AWAIT(clients.back().close());
    }
    CO2_RETURN(stuck);
}
CO2_END

TEST_CASE("Tcp: cancel connect (stop-first, and in flight via timeout)") {
    uvco::Loop loop;
    Tcp server{loop};
    SocketAddress const addr = listenLocal(server);

    // 先停止规则：connect 根本没有发起，流仍可用。
    Tcp tcp{loop};
    co2::stop_source stopped;
    stopped.request_stop();
    Result<void> r = uvco::run(loop, connectOnce(tcp, addr), stopped.get_token());
    CHECK(not r && r.error() == Errc::canceled);
    CHECK(uvco::run(loop, connectOnce(tcp, addr)));

    Tcp tiny{loop};
    SocketAddress const tinyAddr = listenLocal(tiny, 1);
    std::vector<Tcp> clients;
    bool const stuck = uvco::run(loop, connectUntilStuck(loop, tinyAddr, clients));
    if (not stuck)
        std::printf("  note: no connect hung after %zu attempts; in-flight cancel not exercised\n",
                    clients.size());
}

// ---------------------------------------------------------------- close 时的挂起操作

auto pendingRead(Tcp& s, std::vector<char>& buf)
    CO2_BEG(co2::Task<Result<std::size_t>>, (s, buf), Result<std::size_t> n;) {
    CO2_AWAIT_SET(n, s.read(uvco::buffer(buf)));
    CO2_RETURN(n);
}
CO2_END

auto pendingAccept(Tcp& server) CO2_BEG(co2::Task<Errc>, (server), Result<Tcp> c;) {
    CO2_AWAIT_SET(c, server.accept());
    CHECK(not c);
    CO2_RETURN(c.error());
}
CO2_END

auto pendingWrite(Tcp& s, std::vector<char>& big)
    CO2_BEG(co2::Task<Result<void>>, (s, big), Result<void> r;) {
    CO2_AWAIT_SET(r, s.write(uvco::buffer(big)));
    CO2_RETURN(r);
}
CO2_END

auto closer(uvco::Loop& loop, Tcp& client, Tcp& server)
    CO2_BEG(co2::Task<>, (loop, client, server), Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 30ms));
    CHECK(client.writeQueueSize() > 0); // 对端不读：大写排队中
    CO2_AWAIT(client.close());
    CO2_AWAIT(server.close());
}
CO2_END

auto closeScenario(uvco::Loop& loop, Tcp& server, Tcp& client, Tcp& conn, std::vector<char>& buf,
                   std::vector<char>& big)
    CO2_BEG(co2::Task<>, (loop, server, client, conn, buf, big),
            std::tuple<Result<std::size_t>, Result<void>, Errc> rs;) {
    CO2_AWAIT(connectPair(server, client, conn));
    CO2_AWAIT_SET(rs, co2::whenAll(pendingRead(client, buf), pendingWrite(client, big),
                                   pendingAccept(server), closer(loop, client, server)));
    CHECK(not std::get<0>(rs) && std::get<0>(rs).error() == Errc::canceled);
    CHECK(not std::get<1>(rs) && std::get<1>(rs).error() == Errc::canceled);
    CHECK(std::get<2>(rs) == Errc::canceled);
}
CO2_END

TEST_CASE("Tcp: close() with pending read, write and accept -> ECANCELED") {
    uvco::Loop loop;
    Tcp server{loop};
    Tcp client{loop};
    Tcp conn;
    listenLocal(server);
    std::vector<char> buf(64);
    std::vector<char> big(64 * 1024 * 1024, 'z');
    uvco::run(loop, closeScenario(loop, server, client, conn, buf, big));
    CHECK(not client.isOpen() && not server.isOpen() && conn.isOpen());
}

} // namespace

UVCO_TEST_MAIN
