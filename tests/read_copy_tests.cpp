#include "test.hpp"
#include <uvco/uvco.hpp>
#include <cstring>
#include <stdexcept>
namespace {
std::size_t appended = 0;
bool tracking = false;
}
std::string &realAppend(std::string *, char const *,
                        std::size_t) asm("__real_" UVCO_TEST_APPEND_SYMBOL);
std::string &wrappedAppend(std::string *, char const *,
                           std::size_t) asm("__wrap_" UVCO_TEST_APPEND_SYMBOL);
std::string &wrappedAppend(std::string *self, char const *p, std::size_t n) {
    if (tracking)
        appended += n;
    return realAppend(self, p, n);
}
namespace {
struct Source {
    std::string data = std::string(8192, 'x');
    std::size_t pos = 0;
    bool fail = false;
    uvco::BufferedReader<Source> *reader = nullptr;
    struct Read {
        Source *s;
        uvco::MutableBuffer b;
        bool await_ready() const {
            return true;
        }
        void await_suspend(co2::coroutine_handle<>) const {}
        uvco::Result<std::size_t> await_resume() {
            CHECK(s->reader->bufferedBytes() == s->pos);
            if (s->fail && s->pos)
                throw std::runtime_error("read");
            if (s->pos == s->data.size())
                return uvco::Errc::eof;
            auto n = (std::min)(b.size, s->data.size() - s->pos);
            std::memcpy(b.data, s->data.data() + s->pos, n);
            s->pos += n;
            return n;
        }
    };
    Read read(uvco::MutableBuffer b) {
        return {this, b};
    }
};
}
TEST_CASE("BufferedReader reads directly into owned tail without staging append") {
    uvco::Loop loop;
    Source s;
    uvco::BufferedReader<Source> reader{s};
    s.reader = &reader;
    appended = 0;
    tracking = true;
    auto r = uvco::run(loop, reader.readToEnd(8192));
    tracking = false;
    CHECK(r);
    CHECK(*r == s.data);
    CHECK(appended == 0);
}
TEST_CASE("BufferedReader rolls back reserved tail after read exception") {
    uvco::Loop loop;
    Source s;
    s.fail = true;
    uvco::BufferedReader<Source> reader{s};
    s.reader = &reader;
    bool caught = false;
    try {
        static_cast<void>(uvco::run(loop, reader.readToEnd(8192)));
    } catch (std::runtime_error const &) {
        caught = true;
    }
    CHECK(caught);
    CHECK(reader.bufferedBytes() == 4096);
    s.fail = false;
    auto r = uvco::run(loop, reader.readToEnd(8192));
    CHECK(r);
    CHECK(*r == s.data);
}
TEST_CASE("capture reads directly into owned output without staging append") {
    uvco::Loop loop;
    uvco::ProcessOptions p;
    p.file = UVCO_TEST_PROCESS_HELPER;
    p.args = {"echo", std::string(8192, 'x')};
    appended = 0;
    tracking = true;
    auto r = uvco::run(loop, uvco::capture(loop, std::move(p)));
    tracking = false;
    CHECK(r);
    CHECK(r->stdoutData.size() >= 8192);
    CHECK(appended == 0);
}
UVCO_TEST_MAIN
