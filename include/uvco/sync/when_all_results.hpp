#pragma once
#include "uvco/sync/when_any.hpp"
namespace uvco { namespace detail {
template <class V> struct AllPart {
    using Type = std::tuple<typename SuccessOf<V>::type>;
    static Type take(co2compat::ResultStorage<V>& slot) { return Type{slot.take()}; }
};
template <class T> struct AllPart<Result<T>> {
    using Type = std::tuple<T>;
    static Type take(co2compat::ResultStorage<Result<T>>& slot) {
        return Type{std::move(slot.get()).value()};
    }
};
template <> struct AllPart<void> {
    using Type = std::tuple<>;
    static Type take(co2compat::ResultStorage<void>&) { return {}; }
};
template <> struct AllPart<Result<void>> {
    using Type = std::tuple<>;
    static Type take(co2compat::ResultStorage<Result<void>>&) { return {}; }
};
template <class S> struct AllRangeValue {
    using Type = std::vector<S>;
    template <class V> static Result<Type> take(std::vector<co2compat::ResultStorage<V>>& slots) {
        Type out; out.reserve(slots.size());
        for (auto& slot : slots) {
            auto part = AllPart<V>::take(slot);
            out.emplace_back(std::move(std::get<0>(part)));
        }
        return Result<Type>{std::move(out)};
    }
};
template <> struct AllRangeValue<void> {
    using Type = void;
    template <class V> static Result<void> take(std::vector<co2compat::ResultStorage<V>>&) { return {}; }
};
template <class... V> class WhenAllTuple {
    static constexpr std::size_t count = sizeof...(V);
    using Indices = std::index_sequence_for<V...>;

  public:
    using Value = decltype(std::tuple_cat(std::declval<typename AllPart<V>::Type>()...));

    explicit WhenAllTuple(co2::Task<V>... tasks) : tasks_{std::move(tasks)...} {}

    WhenAllTuple(WhenAllTuple&&) = default;
    WhenAllTuple(WhenAllTuple const&) = delete;
    WhenAllTuple& operator=(WhenAllTuple const&) = delete;
    WhenAllTuple& operator=(WhenAllTuple&&) = delete;

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
        if (state_.source.stop_requested()) return Result<Value>{Errc::canceled};
        return Result<Value>{take(Indices{})};
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
        using Fn = BranchEnd (WhenAllTuple::*)();
        static Fn const table[] = {&WhenAllTuple::collectOne<I>...};
        return (this->*table[index])();
    }

    template <std::size_t I> void rethrowOne() {
        UVCO_CONTRACT_CHECK(exceptions_[I] != nullptr);
        std::rethrow_exception(exceptions_[I]);
    }
    template <std::size_t... I> void rethrow(std::size_t const index, std::index_sequence<I...>) {
        using Fn = void (WhenAllTuple::*)();
        static Fn const table[] = {&WhenAllTuple::rethrowOne<I>...};
        (this->*table[index])();
    }

    template <std::size_t I> Errc errorOne() noexcept {
        using VI = typename std::tuple_element<I, std::tuple<V...>>::type;
        return errorOf(std::get<I>(slots_), IsResult<VI>{});
    }
    template <std::size_t... I> Errc errorAt(std::size_t const index, std::index_sequence<I...>) {
        using Fn = Errc (WhenAllTuple::*)();
        static Fn const table[] = {&WhenAllTuple::errorOne<I>...};
        return (this->*table[index])();
    }

    template <std::size_t... I> Value take(std::index_sequence<I...>) {
        return std::tuple_cat(AllPart<V>::take(std::get<I>(slots_))...);
    }

    std::tuple<co2::Task<V>...> tasks_;
    std::tuple<::uvco::detail::co2compat::ResultStorage<V>...> slots_;
    std::array<BranchEnd, count> ends_{};
    std::array<std::exception_ptr, count> exceptions_{};
    RaceState state_{false};
    std::array<RaceChild<WhenAllTuple>, count> children_{};
};

template <class V> class WhenAllRange {
  public:
    using Success = typename SuccessOf<V>::type;
    using Value = typename AllRangeValue<Success>::Type;

    explicit WhenAllRange(std::vector<co2::Task<V>> tasks)
        : tasks_(std::move(tasks)), slots_(tasks_.size()), ends_(tasks_.size()),
          exceptions_(tasks_.size()), children_(tasks_.size()) {

    }

    WhenAllRange(WhenAllRange&&) = default;
    WhenAllRange(WhenAllRange const&) = delete;
    WhenAllRange& operator=(WhenAllRange const&) = delete;
    WhenAllRange& operator=(WhenAllRange&&) = delete;

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
            children_[index].arm(this, index);
            ::uvco::detail::co2compat::TaskAccess::arm(tasks_[index], children_[index].handle(),
                                         state_.source.get_token())
                .resume();
        }
        return not state_.finishStart(skipped);
    }

    Result<Value> await_resume() {
        if (state_.stoppedEarly) return Result<Value>{Errc::canceled};
        std::size_t const exception = state_.firstException.load(std::memory_order_acquire);
        if (exception != RaceState::none) {
            UVCO_CONTRACT_CHECK(exceptions_[exception] != nullptr);
            std::rethrow_exception(exceptions_[exception]);
        }
        std::size_t const error = state_.firstError.load(std::memory_order_acquire);
        if (error != RaceState::none) return Result<Value>{errorOf(slots_[error], IsResult<V>{})};
        if (state_.source.stop_requested()) return Result<Value>{Errc::canceled};
        return AllRangeValue<Success>::take(slots_);
    }

    co2::coroutine_handle<> branchDone(std::size_t const index) noexcept {
        ends_[index] = collect(tasks_[index], slots_[index], exceptions_[index]);
        return state_.branchDone(index, ends_[index]);
    }

  private:
    std::vector<co2::Task<V>> tasks_;
    std::vector<::uvco::detail::co2compat::ResultStorage<V>> slots_;
    std::vector<BranchEnd> ends_;
    std::vector<std::exception_ptr> exceptions_;
    RaceState state_{false};
    std::vector<RaceChild<WhenAllRange>> children_;
};

} // namespace detail
// First failure requests sibling stop; all started children drain before delivery.
// Exception > first business error > cancellation. Void values are omitted.
template <class... A>
UVCO_NODISCARD detail::WhenAllTuple<detail::BranchValue<A>...> whenAllResults(A... branches) {
    static_assert(sizeof...(A) != 0U, "use an empty vector for zero branches");
    return detail::WhenAllTuple<detail::BranchValue<A>...>{detail::Branch<A>::make(std::move(branches))...};
}
template <class A>
UVCO_NODISCARD detail::WhenAllRange<detail::BranchValue<A>> whenAllResults(std::vector<A> branches) {
    return detail::WhenAllRange<detail::BranchValue<A>>{detail::toTasks(std::move(branches))};
}
} // namespace uvco
