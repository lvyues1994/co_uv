// 契约违规测试：Event 上仍有等待者时析构它（docs/architecture.md §4.5）。

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <co2/co2.hpp>

#include "uvco/uvco.hpp"

using namespace std::chrono_literals;

namespace {

auto waitEvent(uvco::Event& event) CO2_BEG(co2::Task<>, (event), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, event.wait());
}
CO2_END

auto destroyLater(uvco::Loop& loop, std::unique_ptr<uvco::Event>& event)
    CO2_BEG(co2::Task<>, (loop, event), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 2ms));
    event.reset(); // wait() 仍在等待：必须触发契约违规
}
CO2_END

auto scenario(uvco::Loop& loop, std::unique_ptr<uvco::Event>& event)
    CO2_BEG(co2::Task<>, (loop, event)) {
    CO2_AWAIT(co2::whenAll(waitEvent(*event), destroyLater(loop, event)));
}
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const& violation) {
        std::cout << "contract violation (expected): " << violation.condition << std::endl;
        std::_Exit(EXIT_SUCCESS);
    });
    uvco::Loop loop;
    std::unique_ptr<uvco::Event> event{new uvco::Event{loop}};
    uvco::run(loop, scenario(loop, event));
    std::cerr << "destroying an Event with a pending wait() was not rejected" << std::endl;
    return EXIT_FAILURE;
}
