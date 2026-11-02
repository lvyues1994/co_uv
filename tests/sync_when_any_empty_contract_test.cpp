// 契约违规测试：whenAny 的 vector 重载拿到空 vector（docs/architecture.md §4.5）。

#include <cstdlib>
#include <iostream>
#include <vector>

#include <co2/co2.hpp>

#include "uvco/uvco.hpp"

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const& violation) {
        std::cout << "contract violation (expected): " << violation.condition << std::endl;
        std::_Exit(EXIT_SUCCESS);
    });
    std::vector<co2::Task<int>> none;
    auto race = uvco::whenAny(std::move(none)); // 没有分支就没有胜者：必须触发契约违规
    static_cast<void>(race);
    std::cerr << "an empty whenAny range was not rejected" << std::endl;
    return EXIT_FAILURE;
}
