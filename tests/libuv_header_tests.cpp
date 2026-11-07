#include "uvco/detail/libuv.hpp"
#include "uvco/uvco.hpp"
#include <limits>
#include "test.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#error centralized libuv import must define NOMINMAX before Windows headers
#endif
#endif
TEST_CASE("central libuv import and numeric limits are self contained") {CHECK(UV_VERSION_MAJOR>=1);CHECK((std::numeric_limits<unsigned>::max)()>0);}
UVCO_TEST_MAIN
