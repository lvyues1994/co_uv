#pragma once

#include <cstdio>
#include <uvco/core/result.hpp>

template <class T>
bool expect(uvco::Result<T> const& r, char const* what) {
    if (r) return true;
    std::fprintf(stderr, "uvco example: %s: %s\n", what, r.message());
    return false;
}

inline int fail(char const* what) {
    std::fprintf(stderr, "uvco example: %s\n", what);
    return 1;
}
