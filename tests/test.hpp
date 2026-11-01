#pragma once

// 极简测试框架：TEST_CASE 注册、CHECK 失败即记录并继续当前用例的下一条之前返回。

#include <cstdlib>
#include <exception>
#include <iostream>
#include <vector>

namespace uvco_test {

struct Case {
    char const* name;
    void (*body)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(char const* const name, void (*const body)()) {
        registry().push_back(Case{name, body});
    }
};

struct Failure {};

inline int& failures() {
    static int count = 0;
    return count;
}

[[noreturn]] inline void fail(char const* const expression, char const* const file,
                              int const line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expression << '\n';
    ++failures();
    throw Failure{};
}

inline int runAll() {
    int failed = 0;
    for (auto const& testCase : registry()) {
        std::cout << "[ RUN  ] " << testCase.name << std::endl;
        try {
            testCase.body();
            std::cout << "[  OK  ] " << testCase.name << std::endl;
        } catch (Failure const&) {
            ++failed;
            std::cout << "[ FAIL ] " << testCase.name << std::endl;
        } catch (std::exception const& error) {
            ++failed;
            std::cout << "[ FAIL ] " << testCase.name << " threw: " << error.what()
                      << std::endl;
        }
    }
    std::cout << registry().size() - static_cast<std::size_t>(failed) << "/"
              << registry().size() << " passed" << std::endl;
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace uvco_test

#define UVCO_TEST_CAT2(a, b) a##b
#define UVCO_TEST_CAT(a, b) UVCO_TEST_CAT2(a, b)

#define TEST_CASE(name)                                                                \
    static void UVCO_TEST_CAT(uvcoTestBody, __LINE__)();                               \
    static ::uvco_test::Registrar const UVCO_TEST_CAT(uvcoTestRegistrar, __LINE__){    \
        name, &UVCO_TEST_CAT(uvcoTestBody, __LINE__)};                                 \
    static void UVCO_TEST_CAT(uvcoTestBody, __LINE__)()

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (not(expression)) ::uvco_test::fail(#expression, __FILE__, __LINE__);       \
    } while (false)

#define UVCO_TEST_MAIN                                                                 \
    int main() { return ::uvco_test::runAll(); }
