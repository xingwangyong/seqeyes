#pragma once

// Compatibility header for std::bind1st which was removed in C++17
// This allows external libraries that use std::bind1st to compile with C++17+

#if __cplusplus >= 201703L
#include <functional>

namespace std {
    template <class Fn, class T>
    auto bind1st(Fn f, T val) {
        return [f, val](auto&&... args) {
            return f(val, std::forward<decltype(args)>(args)...);
        };
    }
}
#endif
