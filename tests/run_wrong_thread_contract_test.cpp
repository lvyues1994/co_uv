// 契约违规测试：在非 loop 线程上调用 run() 必须交给 co2 契约处理器。

#include <cstdlib>
#include <iostream>
#include <thread>

#include <co2/co2.hpp>

#include "uvco/core.hpp"

namespace {

auto noop() CO2_BEG(co2::Task<int>, ()) { CO2_RETURN(1); }
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const& violation) {
        std::cout << "contract violation (expected): " << violation.condition << std::endl;
        std::_Exit(EXIT_SUCCESS);
    });
    uvco::Loop loop;
    std::thread other{[&] { static_cast<void>(uvco::run(loop, noop())); }};
    other.join();
    std::cerr << "run() on a foreign thread was not rejected" << std::endl;
    return EXIT_FAILURE;
}
