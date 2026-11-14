#include <type_traits>
#include <utility>
#include "uvco/core/co2_compat.hpp"
using namespace uvco::detail::co2compat;
static_assert(std::is_standard_layout<FrameHeader>::value,"co2 FrameHeader layout changed");
static_assert(std::is_same<decltype(TaskAccess::takeResult(std::declval<co2::Task<int>&>())),int>::value,
              "co2 TaskAccess result interface changed");
using StartResult=decltype(TaskAccess::start(std::declval<co2::Task<int>&>(),
                          std::declval<co2::coroutine_handle<>>(),std::declval<co2::stop_token>()));
using ArmResult=decltype(TaskAccess::arm(std::declval<co2::Task<int>&>(),
                         std::declval<co2::coroutine_handle<>>(),std::declval<co2::stop_token>()));
static_assert(std::is_same<ArmResult,co2::coroutine_handle<>>::value,"TaskAccess::arm changed");
static_assert(std::is_same<decltype(TaskAccess::hasError(std::declval<co2::Task<int> const&>())),bool>::value,
              "TaskAccess::hasError changed");
using RethrowResult=decltype(TaskAccess::rethrowError(std::declval<co2::Task<int>&>()));
static_assert(std::is_same<decltype(HandleAccess::header(std::declval<co2::coroutine_handle<>>())),FrameHeader*>::value,
              "HandleAccess::header changed");
static_assert(sizeof(LateInit<ResultStorage<int>>)>0,"co2 result storage changed");
static_assert(std::is_constructible<co2::stop_source,decltype(co2::nostopstate)>::value,
              "co2 no-state stop_source construction required");
// uvco 帧局部 ReqAwaiter 不可复制。co2 0be5ab2 起对左值 co_await 只在帧里存指针；
// 更早的 co2 会把 awaiter decay 后拷进槽，无法从左值构造不可移动的 awaiter。
struct UvcoImmovableAwaiter {
    UvcoImmovableAwaiter() = default;
    UvcoImmovableAwaiter(UvcoImmovableAwaiter const&) = delete;
    UvcoImmovableAwaiter(UvcoImmovableAwaiter&&) = delete;
    UvcoImmovableAwaiter& operator=(UvcoImmovableAwaiter const&) = delete;
    UvcoImmovableAwaiter& operator=(UvcoImmovableAwaiter&&) = delete;
    bool await_ready() const noexcept { return true; }
    template <class H>
    void await_suspend(H) const noexcept {}
    void await_resume() const noexcept {}
};
using UvcoLvalueAwaiter = co2::detail::AwaiterOf<UvcoImmovableAwaiter&>;
static_assert(sizeof(UvcoLvalueAwaiter) == sizeof(void*),
              "uvco requires co2 that awaits lvalues in place (commit 0be5ab2 or later); "
              "the installed co2 copies lvalue awaiters into the frame");
static_assert(std::is_constructible<UvcoLvalueAwaiter, UvcoImmovableAwaiter&>::value,
              "uvco requires co2 that can form a pointer awaiter from an lvalue");
int main() { FrameHeader h{&noopDestroy,nullptr,false,false}; (void)h; return 0; }
