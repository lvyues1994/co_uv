#pragma once
#include <atomic>
#include <cstdlib>
#include <new>
namespace allocation_test {
static std::atomic<std::size_t> calls{0};
static thread_local bool enabled = false;
inline void begin() {
    calls = 0;
    enabled = true;
}
inline std::size_t end() {
    enabled = false;
    return calls.load();
}
}
#if defined(_MSC_VER)
#define UVCO_TEST_NOINLINE __declspec(noinline)
#else
#define UVCO_TEST_NOINLINE __attribute__((noinline))
#endif
UVCO_TEST_NOINLINE void *operator new(std::size_t n) {
    if (allocation_test::enabled)
        ++allocation_test::calls;
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) {
    return ::operator new(n);
}
UVCO_TEST_NOINLINE void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    ::operator delete(p);
}
void operator delete(void *p, std::size_t) noexcept {
    ::operator delete(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    ::operator delete(p);
}
