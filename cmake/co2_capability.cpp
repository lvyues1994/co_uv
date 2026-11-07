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
int main() { FrameHeader h{&noopDestroy,nullptr,false,false}; (void)h; return 0; }
