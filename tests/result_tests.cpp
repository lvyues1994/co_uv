#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "test.hpp"
#include "uvco/core/result.hpp"

using uvco::Errc;
using uvco::Result;

TEST_CASE("Result<int> holds a value") {
    Result<int> r = 42;
    CHECK(r);
    CHECK(r.hasValue());
    CHECK(r.value() == 42);
    CHECK(*r == 42);
    CHECK(not r.errorCode());
    CHECK(std::string{r.message()}.empty());
}

TEST_CASE("Result<int> holds an error") {
    Result<int> r = Errc::canceled;
    CHECK(not r);
    CHECK(r.error() == Errc::canceled);
    CHECK(uvco::toInt(r.error()) == UV_ECANCELED);
    CHECK(std::string{r.message()} == uv_strerror(UV_ECANCELED));
    CHECK(r.valueOr(7) == 7);
}

TEST_CASE("error_code conversion uses the uv category") {
    std::error_code const code = Errc::no_such_file;
    CHECK(code.value() == UV_ENOENT);
    CHECK(&code.category() == &uvco::uv_category());
    CHECK(std::string{code.category().name()} == "uv");
    CHECK(code.message() == uv_strerror(UV_ENOENT));
    CHECK(code == Errc::no_such_file);

    Result<int> r = Errc::no_such_file;
    CHECK(r.errorCode() == code);
    CHECK(std::string{uvco::errorName(Errc::no_such_file)} == "ENOENT");
}

TEST_CASE("Errc::aborted is uvco's own code, distinct from canceled") {
    CHECK(Errc::aborted != Errc::canceled);
    CHECK(uvco::toInt(Errc::aborted) == UVCO_EABORTED);
    CHECK(std::string{uvco::errorName(Errc::aborted)} == "UVCO_EABORTED");
    Result<void> r = Errc::aborted;
    CHECK(std::string{r.message()}.find("aborted") != std::string::npos);
    std::error_code const code = Errc::aborted;
    CHECK(code.message() == r.message());
    CHECK(uvco::fromStatus(UVCO_EABORTED).error() == Errc::aborted);
}

TEST_CASE("Result<void>") {
    Result<void> ok;
    CHECK(ok);
    ok.value();
    CHECK(not ok.errorCode());

    Result<void> bad = Errc::eof;
    CHECK(not bad);
    CHECK(bad.error() == Errc::eof);
    CHECK(std::string{bad.message()} == uv_strerror(UV_EOF));

    CHECK(uvco::fromStatus(0));
    CHECK(uvco::fromStatus(UV_EINVAL).error() == Errc::invalid_argument);
}

TEST_CASE("fromValueOrStatus") {
    auto const n = uvco::fromValueOrStatus<std::size_t>(128);
    CHECK(n && *n == 128U);
    auto const e = uvco::fromValueOrStatus<std::size_t>(UV_EBADF);
    CHECK(not e && uvco::toInt(e.error()) == UV_EBADF);
}

TEST_CASE("Result copies, moves and reassigns between states") {
    Result<std::string> a = std::string{"hello"};
    Result<std::string> b = a;
    CHECK(b && *b == "hello");
    CHECK(b->size() == 5U);

    b = Result<std::string>{Errc::broken_pipe};
    CHECK(not b && b.error() == Errc::broken_pipe);

    b = a;
    CHECK(b && *b == "hello");

    Result<std::string> c = std::move(a);
    CHECK(c && *c == "hello");

    a = Errc::eof;
    c = a;
    CHECK(not c && c.error() == Errc::eof);
}

TEST_CASE("Result of a move-only type") {
    Result<std::unique_ptr<int>> r = std::unique_ptr<int>{new int{5}};
    CHECK(r && **r == 5);
    std::unique_ptr<int> owned = std::move(r).value();
    CHECK(owned && *owned == 5);

    Result<std::unique_ptr<int>> moved = std::move(r);
    CHECK(moved);
    moved = Errc::again;
    CHECK(not moved);
}

namespace {
struct ThrowsCopy {
 int bits=-7;
 ThrowsCopy()=default;
 ThrowsCopy(ThrowsCopy const&) {bits=123; throw 1;}
 ThrowsCopy& operator=(ThrowsCopy const&)=default;
};
}
TEST_CASE("Result regression: throwing transition leaves a valid error") {
 uvco::Result<ThrowsCopy> from; uvco::Result<ThrowsCopy> to{uvco::Errc::canceled};
 try {to=from;CHECK(false);}catch(int){}
 CHECK(not to && to.error()==uvco::Errc::no_memory);
}
static_assert(not std::is_copy_constructible<uvco::Result<std::unique_ptr<int>>>::value,
              "move-only Result must advertise noncopyability");
static_assert(not std::is_copy_assignable<uvco::Result<std::unique_ptr<int>>>::value,
              "move-only Result must advertise noncopyability");
UVCO_TEST_MAIN
