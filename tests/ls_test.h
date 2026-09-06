#pragma once
// Minimal check harness. No framework dependency: the engine has none, and the
// tests should not add one.

#include <cstdio>

namespace lstest {

inline int& failures() {
    static int count = 0;
    return count;
}

inline bool check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::printf("FAIL %s:%d  %s\n", file, line, expression);
        ++failures();
    }
    return condition;
}

inline int report(const char* suite) {
    if (failures() == 0) {
        std::printf("%s: all checks passed\n", suite);
        return 0;
    }
    std::printf("%s: %d check(s) failed\n", suite, failures());
    return 1;
}

} // namespace lstest

#define LS_CHECK(...) lstest::check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
// LS_REQUIRE abandons the current test function when a precondition fails, so
// the rest of the suite still runs. LS_REQUIRE_MAIN is its counterpart in main.
#define LS_REQUIRE(...) do { if (!LS_CHECK(__VA_ARGS__)) { return; } } while (false)
#define LS_REQUIRE_MAIN(...) do { if (!LS_CHECK(__VA_ARGS__)) { return lstest::report("aborted"); } } while (false)
