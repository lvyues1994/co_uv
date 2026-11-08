#include <new>
#include <stdexcept>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
int mode = 0, fault = 0;
bool armed = false;
uv_read_cb nextRead = nullptr;
}
// Allocation still happens in libstdc++; intercept tail growth, not the removed append.
void realResize(std::string *, std::size_t) asm("__real_" UVCO_TEST_RESIZE_SYMBOL);
void wrappedResize(std::string *, std::size_t) asm("__wrap_" UVCO_TEST_RESIZE_SYMBOL);
void wrappedResize(std::string *self, std::size_t n) {
    if (n > self->size()) {
        int m = fault;
        fault = 0;
        if (m == 1)
            throw std::bad_alloc{};
        if (m == 2)
            throw std::runtime_error{"output"};
    }
    realResize(self, n);
}
extern "C" int __real_uv_read_start(uv_stream_t *, uv_alloc_cb, uv_read_cb);
namespace {
void callback(uv_stream_t *stream, ssize_t n, uv_buf_t const *b) {
    if (n > 0 && armed) {
        armed = false;
        fault = mode;
    }
    nextRead(stream, n, b);
    fault = 0;
}
}
extern "C" int __wrap_uv_read_start(uv_stream_t *stream, uv_alloc_cb alloc, uv_read_cb read) {
    nextRead = read;
    return __real_uv_read_start(stream, alloc, &callback);
}
TEST_CASE("capture output allocation exceptions drain IO and reap before rethrow") {
    for (int m : {1, 2}) {
        uvco::Loop loop;
        uvco::ProcessOptions p;
        p.file = UVCO_TEST_PROCESS_HELPER;
        p.args = {"duplex"};
        uvco::CaptureOptions io;
        io.input = std::string(1048576, 'x');
        io.cancel = uvco::CaptureCancel::killChild;
        mode = m;
        armed = true;
        bool caught = false;
        try {
            static_cast<void>(uvco::run(loop, uvco::capture(loop, std::move(p), std::move(io))));
        } catch (std::bad_alloc const &) {
            caught = m == 1;
        } catch (std::runtime_error const &) {
            caught = m == 2;
        }
        CHECK(caught);
        CHECK(!armed);
        int pending = 0;
        uv_walk(
            loop.raw(),
            [](uv_handle_t *h, void *p) {
                if (h->type == UV_PROCESS)
                    ++*static_cast<int *>(p);
            },
            &pending);
        CHECK(pending == 0);
        loop.runUntilIdle();
    }
}
UVCO_TEST_MAIN
