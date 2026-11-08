#pragma once
#ifndef UVCO_HAS_KCP
#error "KCP is optional: enable UVCO_ENABLE_KCP and link uvco::kcp"
#endif
#include <ikcp.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>
#include "uvco/core/result.hpp"
#include "uvco/stream/buffer.hpp"

namespace uvco {
using KcpMessage = std::vector<char>;
struct KcpOutputSink {
    void* context;
    // Borrowed bytes. Copy only; no I/O, suspension, scheduling or core reentry.
    bool (*tryCopy)(void*, ConstBuffer) noexcept;
};
struct CoreOptions {
    std::size_t mtu = 1200;
    std::size_t messageLimit = 65536;
    int sendWindow = 128;
    int receiveWindow = 128;
    int interval = 20;
    int fastResend = 2;
    bool congestionControl = true;
    bool stream = false;
    std::uint32_t deadLink = 20;
    static CoreOptions turbo() noexcept {
        CoreOptions o;
        o.interval = 10;
        o.congestionControl = false;
        return o;
    }
};

// Pure synchronous protocol state: no Loop, clock, socket, timer or Task.
// The pinned vendor has unrecoverable OOM paths; see third_party/kcp/VERSION.md.
class KcpCore {
    struct State {
        ikcpcb* kcp = nullptr;
        KcpOutputSink sink;
        CoreOptions options;
        std::size_t limit = 0;
        std::uint64_t drops = 0;
        bool inside = false;
        bool poisoned = false;
        State(CoreOptions o, KcpOutputSink s) noexcept : sink{s}, options{o} {}
        ~State() { if (kcp) ikcp_release(kcp); }
    };
    struct Entry {
        State& state;
        explicit Entry(State& s) noexcept : state{s} {
            UVCO_CONTRACT_CHECK(!s.inside);
            s.inside = true;
        }
        ~Entry() { state.inside = false; }
    };
  public:
    KcpCore(KcpCore&&) noexcept = default;
    KcpCore& operator=(KcpCore&&) noexcept = default;
    KcpCore(KcpCore const&) = delete;
    KcpCore& operator=(KcpCore const&) = delete;
    static Result<KcpCore> create(std::uint32_t conv, CoreOptions options,
                                  KcpOutputSink sink) {
        if (!sink.tryCopy || options.mtu < 50 || options.mtu > 65507 ||
            options.messageLimit == 0 || options.sendWindow < 1 ||
            options.receiveWindow < 128 || options.interval < 10 || options.interval > 5000 ||
            options.fastResend < 0 || options.deadLink < 1)
            return Errc::invalid_argument;
        std::unique_ptr<State> state;
        try { state.reset(new State{options, sink}); }
        catch (std::bad_alloc const&) { return Errc::no_memory; }
        state->kcp = ikcp_create(conv, state.get());
        if (!state->kcp) return Errc::no_memory;
        if (ikcp_setmtu(state->kcp, static_cast<int>(options.mtu)) < 0)
            return Errc::no_memory;
        state->limit = (std::min)(options.messageLimit, (options.mtu - 24) * 127);
        ikcp_wndsize(state->kcp, options.sendWindow, options.receiveWindow);
        ikcp_nodelay(state->kcp, 1, options.interval, options.fastResend,
                     options.congestionControl ? 0 : 1);
        state->kcp->stream = options.stream ? 1 : 0;
        state->kcp->dead_link = options.deadLink;
        ikcp_setoutput(state->kcp, &KcpCore::output);
        return KcpCore{std::move(state)};
    }
    std::size_t messageLimit() const noexcept { return state_->limit; }
    std::size_t mtu() const noexcept { return state_->options.mtu; }
    bool streamMode() const noexcept { return state_->options.stream; }
    std::size_t waitingSegments() const noexcept {
        return static_cast<std::size_t>(ikcp_waitsnd(state_->kcp));
    }
    bool failed() const noexcept { return state_->poisoned; }
    bool deadLink() const noexcept {
        return state_->kcp->state == (std::numeric_limits<IUINT32>::max)();
    }
    std::uint64_t outputDrops() const noexcept { return state_->drops; }
    Result<std::size_t> sendMessage(ConstBuffer data) noexcept {
        if (streamMode()) return Errc::invalid_argument;
        return submit(data);
    }
    Result<std::size_t> write(ConstBuffer data) noexcept {
        if (!streamMode()) return Errc::invalid_argument;
        // Empty stream writes are no-ops, not empty messages or EOF.
        if (data.size == 0) return std::size_t{0};
        return submit(data);
    }
    // Whole-datagram validation must precede the mutating native input call.
    Result<void> input(ConstBuffer data) noexcept {
        if (failed()) return static_cast<Errc>(UV_EPROTO);
        if (!validDatagram(data, state_->kcp->conv, state_->options.mtu, streamMode()))
            return static_cast<Errc>(UV_EPROTO);
        Entry guard{*state_};
        int const status = ikcp_input(state_->kcp, data.data, static_cast<long>(data.size));
        if (status < 0) {
            state_->poisoned = true;
            return static_cast<Errc>(UV_EPROTO);
        }
        return {};
    }
    void update(std::uint32_t now) noexcept {
        Entry guard{*state_};
        if (!failed()) ikcp_update(state_->kcp, now);
    }
    void flush() noexcept {
        Entry guard{*state_};
        if (!failed()) ikcp_flush(state_->kcp);
    }
    std::uint32_t check(std::uint32_t now) const noexcept {
        UVCO_CONTRACT_CHECK(!state_->inside);
        return ikcp_check(state_->kcp, now);
    }
    // Modular signed difference without implementation-defined unsigned->signed casts.
    static std::uint32_t delayUntil(std::uint32_t now, std::uint32_t deadline) noexcept {
        std::uint32_t const delta = deadline - now;
        return delta < UINT32_C(0x80000000) ? delta : 0;
    }
    Result<std::size_t> peekMessageSize() const noexcept {
        if (failed()) return static_cast<Errc>(UV_EPROTO);
        int const size = ikcp_peeksize(state_->kcp);
        if (size < 0) return Errc::again;
        return static_cast<std::size_t>(size);
    }
    Result<KcpMessage> recvMessage() {
        if (streamMode()) return Errc::invalid_argument;
        return receiveUnit();
    }
    Result<KcpMessage> recvStreamUnit() {
        if (!streamMode()) return Errc::invalid_argument;
        return receiveUnit();
    }
    static std::uint32_t decode32(char const* p) noexcept {
        auto const* u = reinterpret_cast<unsigned char const*>(p);
        return std::uint32_t{u[0]} | (std::uint32_t{u[1]} << 8) |
               (std::uint32_t{u[2]} << 16) | (std::uint32_t{u[3]} << 24);
    }
    static bool validDatagram(ConstBuffer data, std::uint32_t conv,
                               std::size_t mtu, bool stream = false) noexcept {
        if (!data.data || data.size < 24 || data.size > 65507 || mtu < 24) return false;
        std::size_t pos = 0;
        while (pos < data.size) {
            if (data.size - pos < 24) return false;
            auto p = data.data + pos;
            auto const cmd = static_cast<unsigned char>(p[4]);
            auto const frg = static_cast<unsigned char>(p[5]);
            auto const len = static_cast<std::size_t>(decode32(p + 20));
            if (decode32(p) != conv || cmd < 81 || cmd > 84 ||
                len > data.size - pos - 24 || len > mtu - 24 || frg >= 127 ||
                (cmd != 81 && (len != 0 || frg != 0)) || (stream && frg != 0))
                return false;
            pos += 24 + len;
        }
        return pos == data.size;
    }
  private:
    explicit KcpCore(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
    static int output(char const* bytes, int length, ikcpcb*, void* context) noexcept {
        auto& s = *static_cast<State*>(context);
        bool const copied = s.sink.tryCopy(s.sink.context,
                                          {bytes, static_cast<std::size_t>(length)});
        if (!copied) ++s.drops;
        return copied ? 0 : -1; // vendor may ignore this; caller observes drops, not rollback
    }
    Result<std::size_t> submit(ConstBuffer data) noexcept {
        if (failed()) return static_cast<Errc>(UV_EPROTO);
        if (data.size != 0 && !data.data) return Errc::invalid_argument;
        if (data.size > state_->limit) return static_cast<Errc>(UV_EMSGSIZE);
        Entry guard{*state_};
        int const accepted = ikcp_send(state_->kcp, data.data, static_cast<int>(data.size));
        if (accepted < 0) {
            // Validated size/mode: -2 is allocation failure, potentially after partial mutation.
            state_->poisoned = true;
            return Errc::no_memory;
        }
        if (static_cast<std::size_t>(accepted) != data.size) {
            // Message submissions are atomic at the facade; never blindly retry a prefix.
            state_->poisoned = true;
            return static_cast<Errc>(UV_EPROTO);
        }
        return static_cast<std::size_t>(accepted);
    }
    Result<KcpMessage> receiveUnit() {
        auto size = peekMessageSize();
        if (!size) return size.error();
        if (*size > state_->limit) return static_cast<Errc>(UV_EMSGSIZE);
        KcpMessage message;
        try { message.resize(*size); }
        catch (std::bad_alloc const&) { return Errc::no_memory; }
        Entry guard{*state_};
        int const got = ikcp_recv(state_->kcp, message.data(), static_cast<int>(message.size()));
        if (got < 0 || static_cast<std::size_t>(got) != message.size()) {
            state_->poisoned = true;
            return static_cast<Errc>(UV_EPROTO);
        }
        return message;
    }
    std::unique_ptr<State> state_;
};
} // namespace uvco
