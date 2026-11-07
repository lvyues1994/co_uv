#pragma once

#include "uvco/core/co2_compat.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <new>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <co2/coroutine.hpp>
#include <co2/coroutine_handle.hpp>
#include <co2/env.hpp>
#include <co2/stop_token.hpp>
#include <co2/task.hpp>

#include "uvco/core/config.hpp"
#include "uvco/core/error.hpp"
#include "uvco/core/result.hpp"

// whenAny：并发启动若干分支，**第一个成功者**胜出（docs/architecture.md §4.5）。
//
//   uvco::Result<uvco::WhenAnyResult<int, void, std::string>> r;
//   CO2_AWAIT_SET(r, uvco::whenAny(fetchInt(), uvco::sleep(loop, 1s), ch.recv()));
//   if (not r) ...                       // 某个分支的错误 / ECANCELED
//   else if (r->index() == 0) use(r->get<0>());
//
// 分支：任意 awaitable（co2::Task、uvco::sleep、Channel::recv、句柄操作…），非 Task 的
// awaitable 被包成一个内部 Task（一次帧分配）。分支结果类型 V 的“成功值”S：
//   V = Result<U> → S = U（Result<void> → void）；其他 V → S = V（永远成功）。
//
// 规则（参照 kotatsu when_any）：
//   - 第一个**结束**的分支决定比赛并对其余分支 request_stop()：成功 → 胜出；
//     Result 错误 → 比赛以该错误结束；ECANCELED → 比赛以取消结束。
//   - 结果按优先级：任一分支抛出的异常（按结束顺序取第一个）重抛 > 任一分支的第一个
//     非 ECANCELED 错误（即使它是在胜者产生后、被取消途中失败的）> 第一个结束者的
//     决定（胜者的值或 ECANCELED）。被取消的落败者返回的值被丢弃。
//   - 错误都是 uvco::Errc，因此异构 Result<U> 的错误统一为 Result<WhenAnyResult<...>> 的错误。
//   - 先看取消：父 stop_token 已请求停止时不启动任何分支，返回 ECANCELED。
//     父取消在比赛中途发生时转发给全部分支（通常都以 ECANCELED 结束）。
//   - 结构化完成：**全部已启动的分支结束后**才返回。分支按参数顺序启动；比赛已决出后
//     尚未启动的分支不再启动（直接销毁）。
//   - 分支可以 scheduleOn 到别的线程；等待者在最后一个结束的分支所在线程上恢复。
//
// vector 重载：同类型分支，返回 Result<WhenAnyRangeResult<S>>{index, value}；空 vector 为契约违规。

namespace uvco {

// 异构 whenAny 的结果：胜者下标 + 胜者的成功值（C++14 没有 variant：未初始化存储 + 下标）。
// void 分支只记录下标。只可移动。默认构造为空（index() == npos），只用作协程帧局部变量。
template <class... S> class WhenAnyResult;

template <class T> struct WhenAnyRangeResult {
    std::size_t index;
    T value;
};

template <> struct WhenAnyRangeResult<void> {
    std::size_t index;
};

namespace detail {

template <class T> struct SizeOrOne : std::integral_constant<std::size_t, sizeof(T)> {};
template <> struct SizeOrOne<void> : std::integral_constant<std::size_t, 1U> {};
template <class T> struct AlignOrOne : std::integral_constant<std::size_t, alignof(T)> {};
template <> struct AlignOrOne<void> : std::integral_constant<std::size_t, 1U> {};

constexpr std::size_t maxOf() noexcept { return 1U; }
template <class... N> constexpr std::size_t maxOf(std::size_t const first, N const... rest) noexcept {
    return first > maxOf(rest...) ? first : maxOf(rest...);
}

template <class T> struct AddLRef { using type = T&; };
template <> struct AddLRef<void> { using type = void; };
template <class T> struct AddCLRef { using type = T const&; };
template <> struct AddCLRef<void> { using type = void; };
template <class T> struct AddRRef { using type = T&&; };
template <> struct AddRRef<void> { using type = void; };

template <class T> struct Slot {
    static void move(void* const dst, void* const src) {
        ::new (dst) T(std::move(*static_cast<T*>(src)));
    }
    static void destroy(void* const p) noexcept { static_cast<T*>(p)->~T(); }
};
template <> struct Slot<void> {
    static void move(void*, void*) noexcept {}
    static void destroy(void*) noexcept {}
};

} // namespace detail

template <class... S> class WhenAnyResult {
    static_assert(sizeof...(S) != 0U, "WhenAnyResult needs at least one alternative");

  public:
    static constexpr std::size_t npos = ~std::size_t{0};

    template <std::size_t I> using Element = typename std::tuple_element<I, std::tuple<S...>>::type;

    WhenAnyResult() noexcept = default;
    WhenAnyResult(WhenAnyResult&& other) { moveFrom(other); }
    WhenAnyResult& operator=(WhenAnyResult&& other) {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }
    WhenAnyResult(WhenAnyResult const&) = delete;
    WhenAnyResult& operator=(WhenAnyResult const&) = delete;
    ~WhenAnyResult() { reset(); }

    std::size_t index() const noexcept { return index_; }

    // 前置条件：index() == I，否则为契约违规。void 分支的 get 返回 void。
    template <std::size_t I> typename detail::AddLRef<Element<I>>::type get() & {
        UVCO_CONTRACT_CHECK(index_ == I);
        return access<I>(std::is_void<Element<I>>{});
    }
    template <std::size_t I> typename detail::AddCLRef<Element<I>>::type get() const& {
        UVCO_CONTRACT_CHECK(index_ == I);
        return caccess<I>(std::is_void<Element<I>>{});
    }
    template <std::size_t I> typename detail::AddRRef<Element<I>>::type get() && {
        UVCO_CONTRACT_CHECK(index_ == I);
        return raccess<I>(std::is_void<Element<I>>{});
    }

    // 由 whenAny 填入胜者（库内部使用；前置条件：为空）。
    template <std::size_t I, class... Args> void emplace(Args&&... args) {
        UVCO_CONTRACT_CHECK(index_ == npos);
        construct<I>(std::is_void<Element<I>>{}, std::forward<Args>(args)...);
        index_ = I;
    }

  private:
    template <std::size_t I> Element<I>& access(std::false_type) {
        return *reinterpret_cast<Element<I>*>(bytes_);
    }
    template <std::size_t I> void access(std::true_type) noexcept {}
    template <std::size_t I> Element<I> const& caccess(std::false_type) const {
        return *reinterpret_cast<Element<I> const*>(bytes_);
    }
    template <std::size_t I> void caccess(std::true_type) const noexcept {}
    template <std::size_t I> Element<I>&& raccess(std::false_type) {
        return std::move(*reinterpret_cast<Element<I>*>(bytes_));
    }
    template <std::size_t I> void raccess(std::true_type) noexcept {}

    template <std::size_t I, class... Args> void construct(std::false_type, Args&&... args) {
        ::new (static_cast<void*>(bytes_)) Element<I>(std::forward<Args>(args)...);
    }
    template <std::size_t I> void construct(std::true_type) noexcept {}

    void moveFrom(WhenAnyResult& other) {
        if (other.index_ == npos) return;
        using Move = void (*)(void*, void*);
        static Move const table[] = {&detail::Slot<S>::move...};
        table[other.index_](bytes_, other.bytes_);
        index_ = other.index_;
        other.reset();
    }

    void reset() noexcept {
        if (index_ == npos) return;
        using Destroy = void (*)(void*);
        static Destroy const table[] = {&detail::Slot<S>::destroy...};
        table[index_](bytes_);
        index_ = npos;
    }

    alignas(detail::maxOf(detail::AlignOrOne<S>::value...)) unsigned char
        bytes_[detail::maxOf(detail::SizeOrOne<S>::value...)];
    std::size_t index_{npos};
};

template <class... S> constexpr std::size_t WhenAnyResult<S...>::npos;

template <std::size_t I, class... S>
auto get(WhenAnyResult<S...>& r) -> decltype(r.template get<I>()) {
    return r.template get<I>();
}
template <std::size_t I, class... S>
auto get(WhenAnyResult<S...> const& r) -> decltype(r.template get<I>()) {
    return r.template get<I>();
}
template <std::size_t I, class... S>
auto get(WhenAnyResult<S...>&& r) -> decltype(std::move(r).template get<I>()) {
    return std::move(r).template get<I>();
}

namespace detail {

// ---------------------------------------------------------------- 分支归一化

template <class V> struct IsResult : std::false_type {};
template <class T> struct IsResult<Result<T>> : std::true_type {};

template <class V> struct SuccessOf { using type = V; };
template <class T> struct SuccessOf<Result<T>> { using type = T; };

template <class Awaitable>
using AwaitValueOf = typename std::decay<decltype(
    std::declval<::uvco::detail::co2compat::AwaiterOf<Awaitable>&>().await_resume())>::type;

template <class V, class Awaitable>
auto awaitableTask(Awaitable awaitable)
    CO2_BEG(co2::Task<V>, (awaitable), ::uvco::detail::co2compat::ResultStorage<V> result;) {
    CO2_AWAIT_SET(result, std::move(awaitable));
    CO2_RETURN(result.take());
}
CO2_END

template <class Awaitable>
auto awaitableVoidTask(Awaitable awaitable) CO2_BEG(co2::Task<void>, (awaitable)) {
    CO2_AWAIT(std::move(awaitable));
}
CO2_END

// 分支值类型（SFINAE 友好：非 awaitable 让 whenAny 的变参重载退出重载决议）。
template <class A> AwaitValueOf<A> branchValue(A*);
template <class T> T branchValue(co2::Task<T>*);
template <class A> using BranchValue = decltype(branchValue(static_cast<A*>(nullptr)));

// 任意 awaitable → Task<V>；Task 原样使用（不多一个帧）。
template <class A> struct Branch {
    using Value = BranchValue<A>;
    static co2::Task<Value> make(A awaitable) {
        return makeTask(std::move(awaitable), std::is_void<Value>{});
    }

  private:
    static co2::Task<Value> makeTask(A awaitable, std::false_type) {
        return awaitableTask<Value>(std::move(awaitable));
    }
    static co2::Task<Value> makeTask(A awaitable, std::true_type) {
        return awaitableVoidTask(std::move(awaitable));
    }
};
template <class T> struct Branch<co2::Task<T>> {
    using Value = T;
    static co2::Task<T> make(co2::Task<T> task) noexcept { return task; }
};

// ---------------------------------------------------------------- 分支结果

enum class BranchEnd : unsigned char { notStarted, success, cancelled, error, exception };

template <class V> BranchEnd classify(::uvco::detail::co2compat::ResultStorage<V> const&, std::false_type) noexcept {
    return BranchEnd::success;
}
template <class V> BranchEnd classify(::uvco::detail::co2compat::ResultStorage<V> const& slot, std::true_type) noexcept {
    auto const& r = slot.get();
    if (r) return BranchEnd::success;
    return r.error() == Errc::canceled ? BranchEnd::cancelled : BranchEnd::error;
}

// Result transfer may throw even after the branch Task has completed. Never
// let that exception escape a child continuation: it must still publish its
// completion and release the structured-concurrency remaining count.
template <class V>
BranchEnd collectValue(co2::Task<V>& task, ::uvco::detail::co2compat::ResultStorage<V>& slot) {
    slot = ::uvco::detail::co2compat::TaskAccess::takeResult(task);
    return classify(slot, IsResult<V>{});
}
inline BranchEnd collectValue(co2::Task<void>& task,
                             ::uvco::detail::co2compat::ResultStorage<void>& slot) {
    ::uvco::detail::co2compat::TaskAccess::takeResult(task);
    slot.emplace();
    return BranchEnd::success;
}
template <class V>
BranchEnd collect(co2::Task<V>& task, ::uvco::detail::co2compat::ResultStorage<V>& slot,
                  std::exception_ptr& exception) noexcept {
    try {
        if (::uvco::detail::co2compat::TaskAccess::hasError(task))
            ::uvco::detail::co2compat::TaskAccess::rethrowError(task);
        return collectValue(task, slot);
    } catch (...) {
        exception = std::current_exception();
        return BranchEnd::exception;
    }
}

template <class V> Errc errorOf(::uvco::detail::co2compat::ResultStorage<V>& slot, std::true_type) noexcept {
    return slot.get().error();
}
template <class V> Errc errorOf(::uvco::detail::co2compat::ResultStorage<V>&, std::false_type) noexcept {
    return Errc::canceled; // 不可达：非 Result 分支没有错误
}

// 把胜者的成功值交给 sink(args...)：Result<U> 解包，Result<void> / void 不带参数。
template <class V, class Sink>
void deliverImpl(::uvco::detail::co2compat::ResultStorage<V>& slot, Sink& sink, std::false_type) {
    sink(slot.take());
}
template <class T, class Sink>
void deliverImpl(::uvco::detail::co2compat::ResultStorage<Result<T>>& slot, Sink& sink, std::true_type) {
    sink(std::move(slot.get()).value());
}
template <class Sink>
void deliverImpl(::uvco::detail::co2compat::ResultStorage<Result<void>>&, Sink& sink, std::true_type) {
    sink();
}
template <class V, class Sink> void deliver(::uvco::detail::co2compat::ResultStorage<V>& slot, Sink&& sink) {
    deliverImpl(slot, sink, IsResult<V>{});
}
template <class Sink> void deliver(::uvco::detail::co2compat::ResultStorage<void>&, Sink&& sink) { sink(); }

// ---------------------------------------------------------------- 比赛状态

struct RaceState {
    static constexpr std::size_t none = ~std::size_t{0};

    explicit RaceState(bool const any = true) noexcept : stopOnSuccess{any} {}
    RaceState(RaceState&& other) noexcept : source{std::move(other.source)}, stopOnSuccess{other.stopOnSuccess} {
        UVCO_CONTRACT_CHECK(not other.awaiting);
    }
    RaceState(RaceState const&) = delete;
    RaceState& operator=(RaceState const&) = delete;
    RaceState& operator=(RaceState&&) = delete;

    // 返回 false：父 stop 已请求（先看取消），不启动任何分支。
    bool begin(co2::coroutine_handle<> const awaiting_, co2::stop_token parent,
               std::size_t const count) {
        if (parent.stop_requested()) {
            stoppedEarly = true;
            return false;
        }
        awaiting = awaiting_;
        remaining.store(count + 1U, std::memory_order_relaxed);
        if (parent.stop_possible())
            forwarding.emplace(std::move(parent), ::uvco::detail::co2compat::ForwardStop{&source});
        return true;
    }

    bool decided() const noexcept { return decider.load(std::memory_order_acquire) != none; }

    // 分支结束。记录第一个异常 / 第一个错误；第一个结束者决定比赛并请求其余分支停止；
    // 最后一个结束者拿到等待者。request_stop() 在递减计数之前：返回前等待者不会恢复。
    co2::coroutine_handle<> branchDone(std::size_t const index, BranchEnd const end) noexcept {
        std::size_t expected = none;
        if (end == BranchEnd::exception)
            firstException.compare_exchange_strong(expected, index, std::memory_order_acq_rel);
        expected = none;
        if (end == BranchEnd::error)
            firstError.compare_exchange_strong(expected, index, std::memory_order_acq_rel);
        expected = none;
        if ((stopOnSuccess || end != BranchEnd::success) &&
            decider.compare_exchange_strong(expected, index, std::memory_order_acq_rel))
            source.request_stop();
        if (remaining.fetch_sub(1U, std::memory_order_acq_rel) == 1U) return awaiting;
        return nullptr;
    }

    // 启动方放下自己那份以及未启动分支的份额；返回 true 表示所有分支都已结束。
    bool finishStart(std::size_t const skipped) noexcept {
        return remaining.fetch_sub(1U + skipped, std::memory_order_acq_rel) == 1U + skipped;
    }

    std::atomic<std::size_t> remaining{0U};
    std::atomic<std::size_t> decider{none};
    std::atomic<std::size_t> firstError{none};
    std::atomic<std::size_t> firstException{none};
    co2::coroutine_handle<> awaiting;
    co2::stop_source source;
    ::uvco::detail::co2compat::LateInit<co2::stop_callback<::uvco::detail::co2compat::ForwardStop>> forwarding;
    bool stoppedEarly{false};
    bool stopOnSuccess{true};
};

// 每个分支的续体帧：标准布局，首成员是帧头。
template <class Op> struct RaceChild {
    ::uvco::detail::co2compat::FrameHeader header;
    Op* op;
    std::size_t index;

    co2::coroutine_handle<> handle() noexcept {
        return co2::coroutine_handle<>::from_address(&header);
    }

    static ::uvco::detail::co2compat::FrameHeader* complete(::uvco::detail::co2compat::FrameHeader* const header) {
        static_assert(std::is_standard_layout<RaceChild>::value,
                      "the completion frame must stay standard-layout");
        auto* const self = reinterpret_cast<RaceChild*>(header);
        auto const next = self->op->branchDone(self->index);
        return next ? ::uvco::detail::co2compat::HandleAccess::header(next) : nullptr;
    }

    void arm(Op* const op_, std::size_t const index_) noexcept {
        header = ::uvco::detail::co2compat::FrameHeader{&::uvco::detail::co2compat::noopDestroy, &RaceChild::complete, false,
                                          false};
        op = op_;
        index = index_;
    }
};

// ---------------------------------------------------------------- 异构（定长）

template <class... V> class WhenAnyTuple {
    static constexpr std::size_t count = sizeof...(V);
    using Indices = std::index_sequence_for<V...>;

  public:
    using Value = WhenAnyResult<typename SuccessOf<V>::type...>;

    explicit WhenAnyTuple(co2::Task<V>... tasks) : tasks_{std::move(tasks)...} {}

    WhenAnyTuple(WhenAnyTuple&&) = default;
    WhenAnyTuple(WhenAnyTuple const&) = delete;
    WhenAnyTuple& operator=(WhenAnyTuple const&) = delete;
    WhenAnyTuple& operator=(WhenAnyTuple&&) = delete;

    bool await_ready() const noexcept { return false; }

    template <class Parent> bool await_suspend(co2::coroutine_handle<Parent> const awaiting) {
        if (not state_.begin(awaiting, ::uvco::detail::co2compat::stopTokenOf(awaiting), count)) return false;
        std::size_t skipped = 0U;
        startAll(skipped, Indices{});
        return not state_.finishStart(skipped);
    }

    Result<Value> await_resume() {
        if (state_.stoppedEarly) return Result<Value>{Errc::canceled};
        std::size_t const exception = state_.firstException.load(std::memory_order_acquire);
        if (exception != RaceState::none) rethrow(exception, Indices{});
        std::size_t const error = state_.firstError.load(std::memory_order_acquire);
        if (error != RaceState::none) return Result<Value>{errorAt(error, Indices{})};
        std::size_t const decider = state_.decider.load(std::memory_order_acquire);
        UVCO_CONTRACT_CHECK(decider != RaceState::none);
        if (ends_[decider] == BranchEnd::cancelled) return Result<Value>{Errc::canceled};
        Value value;
        take(decider, value, Indices{});
        return Result<Value>{std::move(value)};
    }

    co2::coroutine_handle<> branchDone(std::size_t const index) noexcept {
        ends_[index] = collectAt(index, Indices{});
        return state_.branchDone(index, ends_[index]);
    }

  private:
    template <std::size_t... I> void startAll(std::size_t& skipped, std::index_sequence<I...>) {
        int const expand[] = {(startOne<I>(skipped), 0)...};
        static_cast<void>(expand);
    }

    template <std::size_t I> void startOne(std::size_t& skipped) {
        if (state_.decided()) { // 比赛已决出：不再启动
            ++skipped;
            return;
        }
        auto& task = std::get<I>(tasks_);
        UVCO_CONTRACT_CHECK(static_cast<bool>(task));
        children_[I].arm(this, I);
        ::uvco::detail::co2compat::TaskAccess::arm(task, children_[I].handle(), state_.source.get_token())
            .resume();
    }

    // Each child owns its exception slot. collect catches transfer/body errors;
    // RaceState records priority and drains all started children before rethrow.
    template <std::size_t I> BranchEnd collectOne() noexcept {
        return collect(std::get<I>(tasks_), std::get<I>(slots_), exceptions_[I]);
    }
    template <std::size_t... I>
    BranchEnd collectAt(std::size_t const index, std::index_sequence<I...>) noexcept {
        using Fn = BranchEnd (WhenAnyTuple::*)();
        static Fn const table[] = {&WhenAnyTuple::collectOne<I>...};
        return (this->*table[index])();
    }

    template <std::size_t I> void rethrowOne() {
        UVCO_CONTRACT_CHECK(exceptions_[I] != nullptr);
        std::rethrow_exception(exceptions_[I]);
    }
    template <std::size_t... I> void rethrow(std::size_t const index, std::index_sequence<I...>) {
        using Fn = void (WhenAnyTuple::*)();
        static Fn const table[] = {&WhenAnyTuple::rethrowOne<I>...};
        (this->*table[index])();
    }

    template <std::size_t I> Errc errorOne() noexcept {
        using VI = typename std::tuple_element<I, std::tuple<V...>>::type;
        return errorOf(std::get<I>(slots_), IsResult<VI>{});
    }
    template <std::size_t... I> Errc errorAt(std::size_t const index, std::index_sequence<I...>) {
        using Fn = Errc (WhenAnyTuple::*)();
        static Fn const table[] = {&WhenAnyTuple::errorOne<I>...};
        return (this->*table[index])();
    }

    template <std::size_t I> struct Emplace {
        Value* value;
        template <class... Args> void operator()(Args&&... args) const {
            value->template emplace<I>(std::forward<Args>(args)...);
        }
    };
    template <std::size_t I> void takeOne(Value& value) {
        deliver(std::get<I>(slots_), Emplace<I>{&value});
    }
    template <std::size_t... I>
    void take(std::size_t const index, Value& value, std::index_sequence<I...>) {
        using Fn = void (WhenAnyTuple::*)(Value&);
        static Fn const table[] = {&WhenAnyTuple::takeOne<I>...};
        (this->*table[index])(value);
    }

    std::tuple<co2::Task<V>...> tasks_;
    std::tuple<::uvco::detail::co2compat::ResultStorage<V>...> slots_;
    std::array<BranchEnd, count> ends_{};
    std::array<std::exception_ptr, count> exceptions_{};
    RaceState state_;
    std::array<RaceChild<WhenAnyTuple>, count> children_{};
};

// ---------------------------------------------------------------- 同类型（vector）

template <class V> class WhenAnyRange {
  public:
    using Success = typename SuccessOf<V>::type;
    using Value = WhenAnyRangeResult<Success>;

    explicit WhenAnyRange(std::vector<co2::Task<V>> tasks)
        : tasks_(std::move(tasks)), entries_(new Entry[tasks_.size()]) {
        UVCO_CONTRACT_CHECK(not tasks_.empty()); // 没有分支就没有胜者
    }

    WhenAnyRange(WhenAnyRange&&) = default;
    WhenAnyRange(WhenAnyRange const&) = delete;
    WhenAnyRange& operator=(WhenAnyRange const&) = delete;
    WhenAnyRange& operator=(WhenAnyRange&&) = delete;

    bool await_ready() const noexcept { return false; }

    template <class Parent> bool await_suspend(co2::coroutine_handle<Parent> const awaiting) {
        if (not state_.begin(awaiting, ::uvco::detail::co2compat::stopTokenOf(awaiting), tasks_.size()))
            return false;
        std::size_t skipped = 0U;
        for (std::size_t index = 0; index != tasks_.size(); ++index) {
            if (state_.decided()) {
                skipped += tasks_.size() - index;
                break;
            }
            UVCO_CONTRACT_CHECK(static_cast<bool>(tasks_[index]));
            entries_[index].child.arm(this, index);
            ::uvco::detail::co2compat::TaskAccess::arm(tasks_[index], entries_[index].child.handle(),
                                         state_.source.get_token())
                .resume();
        }
        return not state_.finishStart(skipped);
    }

    Result<Value> await_resume() {
        if (state_.stoppedEarly) return Result<Value>{Errc::canceled};
        std::size_t const exception = state_.firstException.load(std::memory_order_acquire);
        if (exception != RaceState::none) {
            UVCO_CONTRACT_CHECK(entries_[exception].exception != nullptr);
            std::rethrow_exception(entries_[exception].exception);
        }
        std::size_t const error = state_.firstError.load(std::memory_order_acquire);
        if (error != RaceState::none) return Result<Value>{errorOf(entries_[error].slot, IsResult<V>{})};
        std::size_t const decider = state_.decider.load(std::memory_order_acquire);
        UVCO_CONTRACT_CHECK(decider != RaceState::none);
        if (entries_[decider].end == BranchEnd::cancelled) return Result<Value>{Errc::canceled};
        Result<Value> out{Errc::canceled};
        deliver(entries_[decider].slot, Make{&out, decider});
        return out;
    }

    co2::coroutine_handle<> branchDone(std::size_t const index) noexcept {
        entries_[index].end = collect(tasks_[index], entries_[index].slot, entries_[index].exception);
        return state_.branchDone(index, entries_[index].end);
    }

  private:
    struct Make {
        Result<Value>* out;
        std::size_t index;
        template <class... Args> void operator()(Args&&... args) const {
            *out = Result<Value>{Value{index, std::forward<Args>(args)...}};
        }
    };

    std::vector<co2::Task<V>> tasks_;
    struct Entry {
        ::uvco::detail::co2compat::ResultStorage<V> slot;
        BranchEnd end=BranchEnd::notStarted;
        std::exception_ptr exception;
        RaceChild<WhenAnyRange> child{};
    };
    std::unique_ptr<Entry[]> entries_;
    RaceState state_;
};

template <class V> std::vector<co2::Task<V>> toTasks(std::vector<co2::Task<V>> tasks) {
    return tasks;
}
template <class A>
std::vector<co2::Task<typename Branch<A>::Value>> toTasks(std::vector<A> awaitables) {
    std::vector<co2::Task<typename Branch<A>::Value>> tasks;
    tasks.reserve(awaitables.size());
    for (auto& a : awaitables)
        tasks.push_back(Branch<A>::make(std::move(a)));
    return tasks;
}

} // namespace detail

// 异构：任意 awaitable（按值接收；Task 须移入）。
template <class... A>
UVCO_NODISCARD detail::WhenAnyTuple<detail::BranchValue<A>...> whenAny(A... branches) {
    static_assert(sizeof...(A) != 0U, "uvco::whenAny needs at least one branch");
    return detail::WhenAnyTuple<detail::BranchValue<A>...>{
        detail::Branch<A>::make(std::move(branches))...};
}

// 同类型的一组分支（非空）。
template <class A>
UVCO_NODISCARD detail::WhenAnyRange<detail::BranchValue<A>> whenAny(std::vector<A> branches) {
    return detail::WhenAnyRange<detail::BranchValue<A>>{
        detail::toTasks(std::move(branches))};
}

} // namespace uvco
