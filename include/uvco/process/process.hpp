#pragma once

#include "uvco/core/co2_compat.hpp"

#include <cstdint>
#include <climits>
#include <string>
#include <utility>
#include <vector>
#include <uv.h>
#include <co2/env.hpp>
#include "uvco/core/handle.hpp"
#include "uvco/core/cancel.hpp"
#include "uvco/core/result.hpp"
#include "uvco/stream/pipe.hpp"

namespace uvco {

struct ExitStatus {
    std::int64_t exit_status = 0;
    int term_signal = 0;
};

// Pipe direction is from the child's perspective. The caller owns the parent Pipe,
// which must be initialized on the same Loop and unopened before spawn.
struct ProcessStdio {
    uv_stdio_flags flags = UV_IGNORE;
    int fd = -1;
    Pipe* pipe = nullptr;
    static ProcessStdio ignore() noexcept { return {}; }
    static ProcessStdio inherit(int fd) noexcept { return {UV_INHERIT_FD, fd, nullptr}; }
    static ProcessStdio inherit(Pipe& p) noexcept { return {UV_INHERIT_STREAM, -1, &p}; }
    static ProcessStdio createPipe(Pipe& p, bool childReads, bool childWrites) noexcept {
        int f = UV_CREATE_PIPE;
        if (childReads) f |= UV_READABLE_PIPE;
        if (childWrites) f |= UV_WRITABLE_PIPE;
        return {static_cast<uv_stdio_flags>(f), -1, &p};
    }
};

struct ProcessOptions {
    std::string file;
    // Arguments exclude argv[0], which is always file.
    std::vector<std::string> args;
    // Empty means inherit environment; set inheritEnv=false for an empty environment.
    std::vector<std::string> env;
    bool inheritEnv = true;
    std::string cwd;
    std::vector<ProcessStdio> stdio; // default: ignore stdin/stdout/stderr
    bool detached = false;
    bool setUid = false;
    bool setGid = false;
    uv_uid_t uid = 0;
    uv_gid_t gid = 0;
};

namespace detail {
class ProcessBlock final : public HandleBlock {
  public:
    explicit ProcessBlock(Loop& loop) : HandleBlock{loop} {}
    uv_process_t handle{};
    int start(uv_process_options_t& options) {
        options.exit_cb = &ProcessBlock::onExit;
        int const status = uv_spawn(loop().raw(), &handle, &options);
        bindHandle();
        return status;
    }
    bool begin(co2::coroutine_handle<> waiter, bool stopped) {
        UVCO_CONTRACT_CHECK(not active_);
        active_ = true;
        opBegin();
        result_ = closing() ? Result<ExitStatus>{Errc::aborted} :
                  stopped ? Result<ExitStatus>{Errc::canceled} : Result<ExitStatus>{exit_};
        if (closing() || stopped || exited_) return false;
        waiter_ = waiter;
        waiting_ = true;
        return true;
    }
    void cancel() noexcept { finish(Errc::canceled, false); }
    Result<ExitStatus> take() {
        auto result = result_;
        active_ = false;
        opEnd();
        return result;
    }
    bool exited() const noexcept { return exited_; }
    void abandon() noexcept {
        UVCO_CONTRACT_CHECK(not hasPendingOps());
        if (orphaned() || closing() || exited_) { static_cast<void>(release({})); return; }
        UVCO_CONTRACT_CHECK(loop().isLoopThread());
        reapOnly_ = true;
        relinquishOwnership(); // still resident and ref'ed until onExit closes it
    }
  private:
    void loopClosing() noexcept override { beginClose(); }
    void beginClose() noexcept override {
        if (closing()) return;
        teardown_ = true;
        finish(Errc::aborted, false);
        // No implicit kill and no zombie: teardown may block until child exits.
        if (exited_) closeInternally();
    }
    uv_handle_t* uvHandle() noexcept override { return reinterpret_cast<uv_handle_t*>(&handle); }
    void abortPending() noexcept override { finish(Errc::aborted, false); }
    void finish(Result<ExitStatus> result, bool inlineResume) noexcept {
        if (not waiting_) return;
        waiting_ = false;
        result_ = result;
        auto waiter = std::exchange(waiter_, {});
        if (inlineResume) waiter.resume();
        else resumeLater(waiter);
    }
    static void onExit(uv_process_t* h, std::int64_t status, int signal) {
        auto& self = fromData<ProcessBlock>(h->data);
        self.exit_ = ExitStatus{status, signal};
        self.exited_ = true;
        if (self.reapOnly_ || self.teardown_) { self.closeInternally(); return; }
        self.finish(self.exit_, true); // no self access after resume
    }
    bool reapOnly_ = false;
    bool teardown_ = false;
    bool active_ = false;
    bool exited_ = false;
    bool waiting_ = false;
    ExitStatus exit_;
    Result<ExitStatus> result_;
    co2::coroutine_handle<> waiter_;
};

class ProcessWaitAwaiter {
  public:
    explicit ProcessWaitAwaiter(ProcessBlock& b) : block_{&b} {}
    ProcessWaitAwaiter(ProcessWaitAwaiter const&) = delete;
    ProcessWaitAwaiter(ProcessWaitAwaiter&& other) noexcept : block_{other.block_} {
        UVCO_CONTRACT_CHECK(not other.started_);
    }
    ~ProcessWaitAwaiter() { UVCO_CONTRACT_CHECK(not started_); }
    bool await_ready() const noexcept { return false; }
    template<class Promise> bool await_suspend(co2::coroutine_handle<Promise> waiter) {
        started_ = true;
        token_ = ::uvco::detail::co2compat::stopTokenOf(waiter);
        bool const pending = block_->begin(waiter, token_.stop_requested());
        if (pending) hook_.arm(block_->loop(), std::move(token_), &cancel, block_);
        return pending;
    }
    Result<ExitStatus> await_resume() {
        hook_.disarm();
        started_ = false;
        return block_->take();
    }
  private:
    static void cancel(void* b) { static_cast<ProcessBlock*>(b)->cancel(); }
    HandleRef<ProcessBlock> block_;
    bool started_ = false;
    co2::stop_token token_;
    CancelHook hook_;
};
} // namespace detail

class Process : public detail::Handle<detail::ProcessBlock> {
    using Base = detail::Handle<detail::ProcessBlock>;
  public:
    Process() noexcept = default;
    Process(Process&&) noexcept = default;
    Process& operator=(Process&& other) noexcept {
        if(this != &other) { abandon(); Base::operator=(std::move(other)); }
        return *this;
    }
    ~Process() { abandon(); }
    // Stop monitoring explicitly; caller is responsible for eventual OS reaping.
    uv_pid_t detach() {
        auto id=pid();
        block().closeInternally();
        {auto c=close();}
        return id;
    }
    // Destruction hands a running child to a ref'ed reap-only block; close aborts
    // pending wait and completes only after exit/reap. Neither sends a signal.
    // Loop teardown also waits for non-detached children, potentially forever.
    // Pending wait must finish or be explicitly aborted before owner destruction.
    detail::ProcessWaitAwaiter wait() { return detail::ProcessWaitAwaiter{block()}; }
    Result<void> kill(int signal) {
        auto& b = block();
        if (b.closing()) return Errc::aborted;
        if (b.exited()) return static_cast<Errc>(UV_ESRCH);
        return fromStatus(uv_process_kill(&b.handle, signal));
    }
    uv_pid_t pid() const { return block().handle.pid; }
  private:
    void abandon() noexcept { auto* b=takeBlock(); if(b) b->abandon(); }
    explicit Process(detail::ProcessBlock* b) : Base{b} {}
    friend Result<Process> spawn(Loop&, ProcessOptions);
};

inline Result<Process> spawn(Loop& loop, ProcessOptions options) {
    UVCO_CONTRACT_CHECK(loop.isLoopThread());
    if (options.file.empty() || options.file.find('\0') != std::string::npos)
        return Errc::invalid_argument;
    std::vector<char*> args;
    args.push_back(&options.file[0]);
    for (auto& a : options.args) {
        if (a.find('\0') != std::string::npos) return Errc::invalid_argument;
        args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);
    std::vector<char*> env;
    for (auto& e : options.env) {
        if (e.find('\0') != std::string::npos) return Errc::invalid_argument;
        env.push_back(const_cast<char*>(e.c_str()));
    }
    env.push_back(nullptr);
    if (options.stdio.size() > static_cast<std::size_t>(INT_MAX)) return Errc::invalid_argument;
    std::vector<uv_stdio_container_t> stdio(options.stdio.size());
    for (std::size_t i = 0; i < stdio.size(); ++i) {
        auto const& s = options.stdio[i];
        int const flags = static_cast<int>(s.flags);
        int const type = flags & (UV_CREATE_PIPE | UV_INHERIT_FD | UV_INHERIT_STREAM);
        int const directions = flags & (UV_READABLE_PIPE | UV_WRITABLE_PIPE);
        if ((flags & ~(UV_CREATE_PIPE | UV_INHERIT_FD | UV_INHERIT_STREAM |
                       UV_READABLE_PIPE | UV_WRITABLE_PIPE)) != 0 ||
            (type != UV_IGNORE && type != UV_CREATE_PIPE && type != UV_INHERIT_FD &&
             type != UV_INHERIT_STREAM) ||
            (type == UV_CREATE_PIPE ? directions == 0 : directions != 0))
            return Errc::invalid_argument;
        stdio[i].flags = s.flags;
        if ((s.flags & (UV_CREATE_PIPE | UV_INHERIT_STREAM)) != 0) {
            if (s.pipe == nullptr || not s.pipe->isOpen() || &s.pipe->loop() != &loop)
                return Errc::invalid_argument;
            stdio[i].data.stream = reinterpret_cast<uv_stream_t*>(s.pipe->raw());
        } else stdio[i].data.fd = s.fd;
    }
    if (options.cwd.find('\0') != std::string::npos) return Errc::invalid_argument;
    uv_process_options_t raw{};
    raw.file = options.file.c_str();
    raw.args = args.data();
    raw.env = options.inheritEnv && options.env.empty() ? nullptr : env.data();
    raw.cwd = options.cwd.empty() ? nullptr : options.cwd.c_str();
    raw.stdio_count = static_cast<int>(stdio.size());
    raw.stdio = stdio.data();
    if (options.detached) raw.flags |= UV_PROCESS_DETACHED;
    if (options.setUid) raw.flags |= UV_PROCESS_SETUID;
    if (options.setGid) raw.flags |= UV_PROCESS_SETGID;
    raw.uid = options.uid;
    raw.gid = options.gid;
    auto* b = new detail::ProcessBlock{loop};
    int const status = b->start(raw);
    if (status < 0) {
        b->closeInternally(); // no child exists, hence no exit callback to await
        static_cast<void>(b->release({}));
        return static_cast<Errc>(status);
    }
    return Process{b};
}
} // namespace uvco
