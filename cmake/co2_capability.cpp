#include <type_traits>
#include <utility>
#include "uvco/core/co2_compat.hpp"
using namespace uvco::detail::co2compat;
static_assert(std::is_standard_layout<FrameHeader>::value,"co2 FrameHeader layout changed");
static_assert(std::is_same<decltype(TaskAccess::takeResult(std::declval<co2::Task<int>&>())),int>::value,
              "co2 TaskAccess result interface changed");
using StartResult=decltype(TaskAccess::start(std::declval<co2::Task<int>&>(),
                          std::declval<co2::coroutine_handle<>>(),std::declval<co2::stop_token>()));
int main() { FrameHeader h{&noopDestroy,nullptr,false,false}; (void)h; return 0; }
