#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <uv.h>

#include <co2/co2.hpp>

#include "test.hpp"
#include "uvco/uvco.hpp"

using namespace std::chrono_literals;
using uvco::Datagram;
using uvco::Errc;
using uvco::Result;
using uvco::SocketAddress;
using uvco::Udp;

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

SocketAddress bindLocal(Udp& udp) {
    CHECK(udp.bind(loopback()));
    Result<SocketAddress> a = udp.localAddress();
    CHECK(a && a->port() != 0);
    return *a;
}

bool receiving(Udp const& udp) {
    return uv_is_active(reinterpret_cast<uv_handle_t const*>(udp.raw())) != 0;
}

auto recvOnce(Udp& udp, std::vector<char>& buf)
    CO2_BEG(co2::Task<Result<Datagram>>, (udp, buf), Result<Datagram> d{Errc::canceled};) {
    CO2_AWAIT_SET(d, udp.recv(uvco::buffer(buf)));
    CO2_RETURN(d);
}
CO2_END

auto sendTo(Udp& udp, SocketAddress to, uvco::ConstBuffer data)
    CO2_BEG(co2::Task<Result<void>>, (udp, to, data), Result<void> r;) {
    CO2_AWAIT_SET(r, udp.send(to, data));
    CO2_RETURN(r);
}
CO2_END

auto sendConnected(Udp& udp, uvco::ConstBuffer data)
    CO2_BEG(co2::Task<Result<void>>, (udp, data), Result<void> r;) {
    CO2_AWAIT_SET(r, udp.send(data));
    CO2_RETURN(r);
}
CO2_END

std::string text(std::vector<char> const& buf, Result<Datagram> const& d) {
    CHECK(d);
    return std::string(buf.data(), d->size);
}

// ---------------------------------------------------------------- ping-pong

auto echoServer(Udp& server, int rounds)
    CO2_BEG(co2::Task<>, (server, rounds), std::vector<char> buf; Result<Datagram> d{Errc::canceled};
            Result<void> r; int i{};) {
    buf.resize(2048);
    for (i = 0; i < rounds; ++i) {
        CO2_AWAIT_SET(d, server.recv(uvco::buffer(buf)));
        CHECK(d && not d->truncated);
        CO2_AWAIT_SET(r, server.send(d->sender, uvco::ConstBuffer{buf.data(), d->size}));
        CHECK(r);
    }
}
CO2_END

auto pingClient(Udp& client, SocketAddress server, int rounds)
    CO2_BEG(co2::Task<int>, (client, server, rounds), std::vector<char> buf; std::string msg;
            Result<Datagram> d{Errc::canceled}; Result<void> r; int i{}; int ok{};) {
    buf.resize(2048);
    for (i = 0; i < rounds; ++i) {
        msg = i == 0 ? std::string{} : "ping-" + std::to_string(i); // 第一个是空数据报
        CO2_AWAIT_SET(r, client.send(server, uvco::buffer(msg)));
        CHECK(r);
        CO2_AWAIT_SET(d, client.recv(uvco::buffer(buf)));
        CHECK(d);
        CHECK(d->sender.toString() == server.toString());
        if (std::string(buf.data(), d->size) == msg) ++ok;
    }
    CO2_RETURN(ok);
}
CO2_END

auto pingPong(Udp& server, Udp& client, SocketAddress addr, int rounds)
    CO2_BEG(co2::Task<int>, (server, client, addr, rounds), std::tuple<int> got;) {
    CO2_AWAIT_SET(got, co2::whenAll(echoServer(server, rounds), pingClient(client, addr, rounds)));
    CO2_RETURN(std::get<0>(got));
}
CO2_END

TEST_CASE("Udp: ping-pong with source address, empty datagram, receive stops when idle") {
    uvco::Loop loop;
    Udp server{loop};
    Udp client{loop};
    SocketAddress const addr = bindLocal(server);
    CHECK(client.bind(loopback()));
    CHECK(uvco::run(loop, pingPong(server, client, addr, 50)) == 50);
    // 没有等待者：推迟的停止已经执行，套接字不再接收，loop 不被它保活。
    CHECK(not receiving(server) && not receiving(client));
    uvco::drain(loop);
}

// ---------------------------------------------------------------- no queue

TEST_CASE("Udp: datagrams that arrive with no waiter stay in the kernel, in order") {
    uvco::Loop loop;
    Udp rx{loop};
    Udp tx{loop};
    SocketAddress const addr = bindLocal(rx);
    std::vector<std::string> const msgs{"one", "two", "three"};
    for (auto const& m : msgs)
        CHECK(uvco::run(loop, sendTo(tx, addr, uvco::buffer(m))));
    CHECK(not receiving(rx));
    std::vector<char> buf(64);
    for (auto const& m : msgs) {
        Result<Datagram> d = uvco::run(loop, recvOnce(rx, buf));
        CHECK(text(buf, d) == m);
        CHECK(d->sender.port() == tx.localAddress()->port()); // tx 自动绑定在 0.0.0.0
    }
    CHECK(not receiving(rx));
}

TEST_CASE("Udp: short buffer truncates one datagram; the next recv gets the next one") {
    uvco::Loop loop;
    Udp rx{loop};
    Udp tx{loop};
    SocketAddress const addr = bindLocal(rx);
    std::string const big(100, 'b');
    std::string const small{"next"};
    CHECK(uvco::run(loop, sendTo(tx, addr, uvco::buffer(big))));
    CHECK(uvco::run(loop, sendTo(tx, addr, uvco::buffer(small))));
    std::vector<char> buf(10);
    Result<Datagram> d = uvco::run(loop, recvOnce(rx, buf));
    CHECK(d && d->size == 10 && d->truncated && text(buf, d) == std::string(10, 'b'));
    d = uvco::run(loop, recvOnce(rx, buf));
    CHECK(d && not d->truncated && text(buf, d) == small);
}

// 接收端每次恢复后立刻再 recv（推迟的停止看到新的等待者，不停止）。
auto burstReceiver(Udp& rx, int count)
    CO2_BEG(co2::Task<int>, (rx, count), std::vector<char> buf; Result<Datagram> d{Errc::canceled};
            int i{}; int inOrder{};) {
    buf.resize(64);
    for (i = 0; i < count; ++i) {
        CO2_AWAIT_SET(d, rx.recv(uvco::buffer(buf)));
        CHECK(d);
        if (std::string(buf.data(), d->size) == std::to_string(i)) ++inOrder;
    }
    CO2_RETURN(inOrder);
}
CO2_END

auto burstSender(Udp& tx, SocketAddress to, int count)
    CO2_BEG(co2::Task<>, (tx, to, count), std::string msg; Result<void> r; int i{};) {
    for (i = 0; i < count; ++i) {
        msg = std::to_string(i);
        CO2_AWAIT_SET(r, tx.send(to, uvco::buffer(msg)));
        CHECK(r);
    }
}
CO2_END

auto burst(Udp& rx, Udp& tx, SocketAddress to, int count)
    CO2_BEG(co2::Task<int>, (rx, tx, to, count), std::tuple<int> got;) {
    CO2_AWAIT_SET(got, co2::whenAll(burstReceiver(rx, count), burstSender(tx, to, count)));
    CO2_RETURN(std::get<0>(got));
}
CO2_END

// 发送端一次发完（try_send 不挂起），接收端逐个取：数据报在内核缓冲区里等待，
// 数量保持在默认接收缓冲区之内（超出时内核丢弃——不排队的代价）。
TEST_CASE("Udp: a burst of datagrams, one recv each, delivered in order") {
    uvco::Loop loop;
    Udp rx{loop};
    Udp tx{loop};
    SocketAddress const addr = bindLocal(rx);
    CHECK(uvco::run(loop, burst(rx, tx, addr, 100)) == 100);
    CHECK(not receiving(rx));
}

// ---------------------------------------------------------------- connect / errors

TEST_CASE("Udp: connect, send(data) to the peer, peerAddress, disconnect") {
    uvco::Loop loop;
    Udp rx{loop};
    Udp tx{loop};
    SocketAddress const addr = bindLocal(rx);
    std::string const msg{"connected"};
    // 未连接时 send(data) 没有目标。
    Result<void> r = uvco::run(loop, sendConnected(tx, uvco::buffer(msg)));
    CHECK(not r);
    CHECK(tx.connect(addr));
    CHECK(tx.peerAddress() && tx.peerAddress()->toString() == addr.toString());
    CHECK(uvco::run(loop, sendConnected(tx, uvco::buffer(msg))));
    std::vector<char> buf(64);
    Result<Datagram> d = uvco::run(loop, recvOnce(rx, buf));
    CHECK(text(buf, d) == msg);
    CHECK(tx.disconnect());
    CHECK(not tx.peerAddress());
    // 地址错误与过大的数据报。
    std::string const huge(70000, 'h');
    r = uvco::run(loop, sendTo(tx, addr, uvco::buffer(huge)));
    CHECK(not r && static_cast<int>(r.error()) == UV_EMSGSIZE);
    r = uvco::run(loop, sendTo(tx, SocketAddress{}, uvco::buffer(msg)));
    CHECK(not r && r.error() == Errc::invalid_argument);
}

TEST_CASE("Udp: ICMP port unreachable on a connected socket -> connection_refused, socket usable") {
    uvco::Loop loop;
    SocketAddress dead;
    {
        Udp probe{loop};
        dead = bindLocal(probe);
    } // 关闭：端口不再有人监听
    uvco::drain(loop);
    Udp udp{loop};
    CHECK(udp.connect(dead));
    std::string const msg{"anyone?"};
    std::vector<char> buf(64);
    CHECK(uvco::run(loop, sendConnected(udp, uvco::buffer(msg))));
    // 错误可能在 recv 挂起之前或之后到达；两种路径都交给这个 recv。
    Result<Datagram> d = uvco::run(loop, recvOnce(udp, buf));
    CHECK(not d && d.error() == Errc::connection_refused);
    CHECK(uvco::run(loop, sendConnected(udp, uvco::buffer(msg))));
    d = uvco::run(loop, recvOnce(udp, buf));
    CHECK(not d && d.error() == Errc::connection_refused);
    // 连向一个活着的对端后照常工作。
    Udp live{loop};
    SocketAddress const addr = bindLocal(live);
    CHECK(udp.disconnect());
    CHECK(udp.connect(addr));
    CHECK(uvco::run(loop, sendConnected(udp, uvco::buffer(msg))));
    d = uvco::run(loop, recvOnce(live, buf));
    CHECK(text(buf, d) == msg);
}

// ---------------------------------------------------------------- cancel / close

auto recvTimeout(uvco::Loop& loop, Udp& rx, Udp& tx, SocketAddress addr, std::vector<char>& buf)
    CO2_BEG(co2::Task<>, (loop, rx, tx, addr, buf), Result<Datagram> d{Errc::canceled};
            Result<void> r; std::string const msg{"late"};) {
    CO2_AWAIT_SET(d, uvco::timeout(loop, 30ms, rx.recv(uvco::buffer(buf))));
    CHECK(not d && d.error() == Errc::timed_out);
    CO2_AWAIT_SET(r, tx.send(addr, uvco::buffer(msg)));
    CHECK(r);
    CO2_AWAIT_SET(d, uvco::timeout(loop, 5s, rx.recv(uvco::buffer(buf))));
    CHECK(d && std::string(buf.data(), d->size) == msg);
}
CO2_END

TEST_CASE("Udp: timeout() around recv -> timed_out, socket stays usable") {
    uvco::Loop loop;
    Udp rx{loop};
    Udp tx{loop};
    SocketAddress const addr = bindLocal(rx);
    std::vector<char> buf(64);
    uvco::run(loop, recvTimeout(loop, rx, tx, addr, buf));
    CHECK(not receiving(rx));
}

TEST_CASE("Udp: recv cancelled from another thread; stop-first even with data ready") {
    uvco::Loop loop;
    Udp rx{loop};
    Udp tx{loop};
    SocketAddress const addr = bindLocal(rx);
    std::vector<char> buf(64);
    for (int i = 0; i < 5; ++i) {
        co2::stop_source source;
        StopLater stopper{source, std::chrono::milliseconds{i * 3}};
        Result<Datagram> d = uvco::run(loop, recvOnce(rx, buf), source.get_token());
        CHECK(not d && d.error() == Errc::canceled);
        CHECK(not receiving(rx));
    }
    std::string const msg{"kept"};
    CHECK(uvco::run(loop, sendTo(tx, addr, uvco::buffer(msg))));
    co2::stop_source stopped;
    stopped.request_stop();
    Result<Datagram> d = uvco::run(loop, recvOnce(rx, buf), stopped.get_token());
    CHECK(not d && d.error() == Errc::canceled);
    d = uvco::run(loop, recvOnce(rx, buf));
    CHECK(text(buf, d) == msg);
}

auto closeLater(uvco::Loop& loop, Udp& rx) CO2_BEG(co2::Task<>, (loop, rx), Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 10ms));
    CHECK(receiving(rx));
    CO2_AWAIT(rx.close());
}
CO2_END

auto closeWhileReceiving(uvco::Loop& loop, Udp& rx, std::vector<char>& buf)
    CO2_BEG(co2::Task<>, (loop, rx, buf), std::tuple<Result<Datagram>> got;) {
    CO2_AWAIT_SET(got, co2::whenAll(recvOnce(rx, buf), closeLater(loop, rx)));
    CHECK(not std::get<0>(got) && std::get<0>(got).error() == Errc::aborted);
}
CO2_END

TEST_CASE("Udp: close() with a pending recv -> aborted") {
    uvco::Loop loop;
    Udp rx{loop};
    bindLocal(rx);
    std::vector<char> buf(64);
    uvco::run(loop, closeWhileReceiving(loop, rx, buf));
    CHECK(not rx.isOpen());
}

TEST_CASE("~Loop ends a pending recv with aborted") {
    std::unique_ptr<uvco::Loop> loop{new uvco::Loop};
    Udp rx{*loop};
    bindLocal(rx);
    std::vector<char> buf(64);
    auto handle = co2::spawn(*loop, recvOnce(rx, buf));
    static_cast<void>(uv_run(loop->raw(), UV_RUN_NOWAIT));
    CHECK(not handle.isReady());
    loop.reset();
    CHECK(handle.isReady());
    Result<Datagram> const d = handle.join();
    CHECK(not d && d.error() == Errc::aborted);
    CHECK(rx.isOpen()); // 孤儿：析构时直接释放
}

TEST_CASE("Udp: destroying the socket right after a delivery (deferred stop still queued)") {
    uvco::Loop loop;
    Udp tx{loop};
    std::vector<char> buf(64);
    std::string const msg{"x"};
    for (int i = 0; i < 20; ++i) {
        Udp rx{loop};
        SocketAddress const addr = bindLocal(rx);
        CHECK(uvco::run(loop, sendTo(tx, addr, uvco::buffer(msg))));
        auto handle = co2::spawn(loop, recvOnce(rx, buf));
        // 跑到 recv 完成为止，但不多跑：推迟的停止任务可能还在队列里。
        while (not handle.isReady())
            static_cast<void>(uv_run(loop.raw(), UV_RUN_ONCE));
        CHECK(text(buf, handle.join()) == msg);
    } // rx 在这里析构；控制块等推迟任务执行完才释放（ASan 检查）
    uvco::drain(loop);
}

} // namespace

UVCO_TEST_MAIN
