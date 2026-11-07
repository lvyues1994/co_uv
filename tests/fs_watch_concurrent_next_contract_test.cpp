#include <cstdlib>
#include <iostream>
#include <string>
#include <co2/co2.hpp>
#include "uvco/uvco.hpp"
namespace {
auto read(uvco::fs::FsPoll& watcher)
    CO2_BEG(co2::Task<>, (watcher), uvco::Result<uvco::fs::FsPollChange> result;) {
    CO2_AWAIT_SET(result, watcher.next());
} CO2_END
auto concurrent(uvco::fs::FsPoll& watcher) CO2_BEG(co2::Task<>, (watcher)) {
    CO2_AWAIT(co2::whenAll(read(watcher), read(watcher)));
} CO2_END
}
int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const& violation) {
        bool const expected = std::string{violation.condition}.find("slot_.idle()") != std::string::npos;
        std::cout << "contract: " << violation.condition << std::endl;
        std::_Exit(expected ? EXIT_SUCCESS : EXIT_FAILURE);
    });
    uvco::Loop loop;
    auto watcher = uvco::fs::FsPoll::start(loop, UVCO_TEST_WATCH_FILE);
    if (!watcher) return EXIT_FAILURE;
    uvco::run(loop, concurrent(*watcher));
    return EXIT_FAILURE;
}
