#include <uvco/kcp.hpp>
#include <uvco/uvco.hpp>
#include "latency_stats.hpp"
#include <array>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>
using namespace uvco;
namespace {
template <class T> void need(T const &result) {
    if (!result)
        throw std::runtime_error("benchmark I/O failed");
}
struct Run {
    std::size_t rounds = 1000, warmup = 100, bytes = 128;
    std::vector<double> samples;
    explicit Run(std::size_t r, std::size_t b) : rounds{r}, bytes{b} {
        samples.reserve(r);
    }
};
void report(char const *label, Run const &r, double cpu) {
    auto s = uvco_bench::summarize(r.samples);
    std::printf("%s rounds=%zu bytes=%zu RTT_us p50=%.3f p99=%.3f max=%.3f CPU_s=%.6f\n", label,
                r.samples.size(), r.bytes, s.p50, s.p99, s.max, cpu);
}
auto kcpServer(KcpSession &s, Run &r)
    CO2_BEG(co2::Task<>,(s,r),std::size_t i=0;Result<KcpMessage> data{Errc::canceled};Result<void> sent;) {
    for (i = 0; i < r.rounds + r.warmup; ++i) {
        CO2_AWAIT_SET(data, s.recvMessage());
        need(data);
        if (data->size() != r.bytes)
            throw std::runtime_error("KCP echo size mismatch");
        CO2_AWAIT_SET(sent, s.sendMessage(std::move(*data)));
        need(sent);
    }
    CO2_RETURN();
}
CO2_END
auto kcpClient(KcpSession &s, Run &r)
    CO2_BEG(co2::Task<>,(s,r),std::size_t i=0;std::uint64_t start=0;KcpMessage data;Result<KcpMessage> echo{Errc::canceled};Result<void> sent;) {
    data.assign(r.bytes, 'x');
    for (i = 0; i < r.rounds + r.warmup; ++i) {
        start = uv_hrtime();
        CO2_AWAIT_SET(sent, s.sendMessage(data));
        need(sent);
        CO2_AWAIT_SET(echo, s.recvMessage());
        need(echo);
        if (*echo != data)
            throw std::runtime_error("KCP echo mismatch");
        if (i >= r.warmup)
            r.samples.push_back(static_cast<double>(uv_hrtime() - start) / 1000.0);
    }
    CO2_RETURN();
}
CO2_END
auto kcpPair(KcpSession &a, KcpSession &b, Run &r) CO2_BEG(co2::Task<>,(a,b,r)) {
    CO2_AWAIT(co2::whenAll(kcpClient(a, r), kcpServer(b, r)));
    CO2_RETURN();
}
CO2_END
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
struct SimPacket {
    std::array<char, 1200> bytes{};
    std::size_t size = 0;
    std::uint64_t due = 0;
};
struct SimWire {
    std::vector<SimPacket> queue = std::vector<SimPacket>(512);
    std::size_t count = 0;
    std::uint64_t now = 0, delay = 0, networkDrops = 0;
    std::uint32_t seed = 12345;
    double loss = 0;
    static bool copy(void *ptr, ConstBuffer data) noexcept {
        auto &w = *static_cast<SimWire *>(ptr);
        w.seed = w.seed * UINT32_C(1664525) + UINT32_C(1013904223);
        if (static_cast<double>(w.seed % 100000) < w.loss * 1000.0) {
            ++w.networkDrops;
            return true;
        }
        if (w.count == w.queue.size() || data.size > 1200)
            return false;
        auto &p = w.queue[w.count++];
        std::memcpy(p.bytes.data(), data.data, data.size);
        p.size = data.size;
        p.due = w.now + w.delay;
        return true;
    }
    bool deliver(KcpCore &to) {
        bool input = false;
        for (std::size_t n = count; n > 0; --n) {
            auto i = n - 1;
            if (queue[i].due <= now) {
                need(to.input({queue[i].bytes.data(), queue[i].size}));
                queue[i] = queue[--count];
                input = true;
            }
        }
        return input;
    }
};
void simulated(Run &r, std::uint64_t delay, double loss, bool immediate) {
    SimWire ab, ba;
    ab.delay = ba.delay = delay;
    ab.loss = ba.loss = loss;
    ba.seed = 67890;
    auto a = KcpCore::create(1, CoreOptions{}, {&ab, &SimWire::copy});
    auto b = KcpCore::create(1, CoreOptions{}, {&ba, &SimWire::copy});
    need(a);
    need(b);
    std::uint64_t now = 0;
    a->update(0);
    b->update(0);
    KcpMessage message(r.bytes, 'x');
    auto cpu = std::clock();
    for (std::size_t i = 0; i < r.rounds + r.warmup; ++i) {
        auto start = now;
        a->update(static_cast<std::uint32_t>(now));
        b->update(static_cast<std::uint32_t>(now));
        need(a->sendMessage(buffer(message)));
        a->flush();
        bool received = false, echoed = false;
        for (; now - start < 120000; ++now) {
            ab.now = ba.now = now;
            a->update(static_cast<std::uint32_t>(now));
            b->update(static_cast<std::uint32_t>(now));
            if (ab.deliver(*b) && immediate)
                b->flush();
            auto request = b->recvMessage();
            if (request) {
                need(b->sendMessage(buffer(*request)));
                b->flush();
                echoed = true;
            }
            if (ba.deliver(*a) && immediate)
                a->flush();
            auto response = a->recvMessage();
            if (response) {
                if (!echoed || *response != message)
                    throw std::runtime_error("simulated echo mismatch");
                received = true;
                break;
            }
            if (a->deadLink() || b->deadLink())
                break;
        }
        if (!received)
            throw std::runtime_error("simulated timeout/dead_link; samples not silently omitted");
        if (i >= r.warmup)
            r.samples.push_back(static_cast<double>(now - start) * 1000.0);
        ++now;
        ab.now = ba.now = now;
    }
    std::printf("SIMULATION ONLY: one_way_delay_ms=%llu loss_percent=%.3f seed=12345/67890 "
                "network_drops=%llu; no matching TCP simulation\n",
                static_cast<unsigned long long>(delay), loss,
                static_cast<unsigned long long>(ab.networkDrops + ba.networkDrops));
    report("KCP-simulated-injected-clock-1ms-NOT-loopback-comparison", r,
           static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC);
}
}
int main(int argc, char **argv) {
    try {
        std::size_t rounds = 1000, bytes = 128;
        std::uint64_t delay = 0;
        double loss = 0;
        bool sim = false, immediate = true;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (i + 1 >= argc)
                throw std::invalid_argument("missing option value");
            std::string value = argv[++i];
            if (arg == "--rounds")
                rounds = std::stoul(value);
            else if (arg == "--bytes")
                bytes = std::stoul(value);
            else if (arg == "--sim-delay-ms") {
                delay = std::stoull(value);
                sim = true;
            } else if (arg == "--sim-loss-percent") {
                loss = std::stod(value);
                sim = true;
            } else if (arg == "--immediate-flush") {
                if (value != "0" && value != "1")
                    throw std::invalid_argument("immediate-flush must be 0 or 1");
                immediate = value == "1";
            } else
                throw std::invalid_argument("unknown option");
        }
        if (rounds == 0 || rounds > 1000000 || bytes == 0 || bytes > 65536 || delay > 10000 ||
            !std::isfinite(loss) || loss < 0 || loss >= 100)
            throw std::invalid_argument("invalid benchmark options");
        std::printf("NATIVE LOOPBACK: sequential fixed-length echo, warmup=100, TCP_NODELAY=ON, "
                    "KCP=1/20/2/0 immediate-input-flush=%s\n",
                    immediate ? "ON" : "OFF");
        auto address = *SocketAddress::ipv4("127.0.0.1", 0);
        {
            Loop loop;
            auto a = KcpEndpoint::create(loop, address), b = KcpEndpoint::create(loop, address);
            need(a);
            need(b);
            KcpSessionOptions options;
            options.immediateFlushAfterInput = immediate;
            auto sa = a->openSession(*b->localAddress(), 1, options),
                 sb = b->openSession(*a->localAddress(), 1, options);
            need(sa);
            need(sb);
            Run r{rounds, bytes};
            auto cpu = std::clock();
            run(loop, kcpPair(*sa, *sb, r));
            report("KCP-native-loopback", r,
                   static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC);
            run(loop, closeH(*sa));
            run(loop, closeH(*sb));
            run(loop, closeH(*a));
            run(loop, closeH(*b));
            drain(loop);
        }
        {
            Loop loop;
            Tcp listener{loop}, client{loop};
            need(listener.bind(address));
            need(listener.listen());
            Run r{rounds, bytes};
            auto cpu = std::clock();
            run(loop, tcpPair(client, listener, *listener.localAddress(), r));
            report("TCP-native-loopback-NODELAY", r,
                   static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC);
            run(loop, closeH(listener));
            drain(loop);
        }
        if (sim) {
            Run r{rounds, bytes};
            simulated(r, delay, loss, immediate);
        }
        return 0;
    } catch (std::exception const &e) {
        std::fprintf(stderr, "benchmark failed: %s\n", e.what());
        return 1;
    }
}
