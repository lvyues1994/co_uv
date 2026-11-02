// 契约违规测试：Loop 析构后（句柄已成为孤儿）继续使用句柄（docs/architecture.md §4.2）。

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <co2/co2.hpp>

#include "uvco/uvco.hpp"

using namespace std::chrono_literals;

int main() {
    std::unique_ptr<uvco::Loop> loop{new uvco::Loop};
    uvco::Timer timer{*loop};
    if (not timer.start(10s)) return EXIT_FAILURE;
    loop.reset();
    co2::setContractViolationHandler([](co2::ContractViolation const& violation) {
        std::cout << "contract violation (expected): " << violation.condition << std::endl;
        std::_Exit(EXIT_SUCCESS);
    });
    timer.stop(); // 孤儿：必须触发契约违规
    std::cerr << "using a Timer after its Loop was destroyed was not rejected" << std::endl;
    return EXIT_FAILURE;
}
