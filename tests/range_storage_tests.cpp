#include <cstddef>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/sync.hpp"
namespace {bool counting=false;int allocations=0;}
extern "C" void* UVCO_TEST_REAL_NEW(std::size_t);
extern "C" void* UVCO_TEST_WRAP_NEW(std::size_t n){if(counting)++allocations;return UVCO_TEST_REAL_NEW(n);}
extern "C" void* UVCO_TEST_REAL_ARRAY_NEW(std::size_t);
extern "C" void* UVCO_TEST_WRAP_ARRAY_NEW(std::size_t n){if(counting)++allocations;return UVCO_TEST_REAL_ARRAY_NEW(n);}
namespace {auto val() CO2_BEG(co2::Task<int>,()){CO2_RETURN(7);} CO2_END}
TEST_CASE("whenAny range has one fixed entry allocation plus stop state") {
 std::vector<co2::Task<int>> tasks;tasks.push_back(val());tasks.push_back(val());
 allocations=0;counting=true;auto race=uvco::whenAny(std::move(tasks));counting=false;
 CHECK(allocations==2);static_cast<void>(race);
}
UVCO_TEST_MAIN
