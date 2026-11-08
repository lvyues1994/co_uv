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
#ifdef UVCO_TEST_WRAP_NEW
// Preserve sanitizer new/delete interceptors rather than replacing them.
void *allocationRealNew(std::size_t) asm("__real_" UVCO_TEST_NEW_SYMBOL);
void *allocationWrapNew(std::size_t) asm("__wrap_" UVCO_TEST_NEW_SYMBOL);
void *allocationRealArray(std::size_t) asm("__real_" UVCO_TEST_NEW_ARRAY_SYMBOL);
void *allocationWrapArray(std::size_t) asm("__wrap_" UVCO_TEST_NEW_ARRAY_SYMBOL);
UVCO_TEST_NOINLINE void *allocationWrapNew(std::size_t n) {
    if (allocation_test::enabled)
        ++allocation_test::calls;
    return allocationRealNew(n);
}
UVCO_TEST_NOINLINE void *allocationWrapArray(std::size_t n) {
    if (allocation_test::enabled)
        ++allocation_test::calls;
    return allocationRealArray(n);
}
#else
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

#endif
