// 契约违规测试：句柄上有挂起中的操作时析构句柄（docs/architecture.md §4.2）。

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <co2/co2.hpp>

#include "uvco/uvco.hpp"

using namespace std::chrono_literals;

namespace {

auto waitTick(uvco::Timer& timer)
    CO2_BEG(co2::Task<>, (timer), uvco::Result<std::uint64_t> r;) {
    CO2_AWAIT_SET(r, timer.tick());
}
CO2_END

auto destroyLater(uvco::Loop& loop, std::unique_ptr<uvco::Timer>& timer)
    CO2_BEG(co2::Task<>, (loop, timer), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 5ms));
    timer.reset(); // tick() 仍在等待：必须触发契约违规
}
CO2_END

auto scenario(uvco::Loop& loop, std::unique_ptr<uvco::Timer>& timer)
    CO2_BEG(co2::Task<>, (loop, timer)) {
    CO2_AWAIT(co2::whenAll(waitTick(*timer), destroyLater(loop, timer)));
}
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const& violation) {
        std::cout << "contract violation (expected): " << violation.condition << std::endl;
        std::_Exit(EXIT_SUCCESS);
    });
    uvco::Loop loop;
    std::unique_ptr<uvco::Timer> timer{new uvco::Timer{loop}};
    if (not timer->start(10s)) return EXIT_FAILURE;
    uvco::run(loop, scenario(loop, timer));
    std::cerr << "destroying a Timer with a pending tick() was not rejected" << std::endl;
    return EXIT_FAILURE;
}
