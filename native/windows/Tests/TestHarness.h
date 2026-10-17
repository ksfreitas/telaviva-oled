#pragma once
// Minimal test registry. Tests register at static init; CHECK failures are
// counted and printed without aborting the test, so one run shows them all.
// CHECK_EQ needs operator<< for both operands.
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace telaviva_test {

struct TestCase {
    const char* name;
    void (*run)();
};

// Function-local statics avoid static-init order problems across translation units.
inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failedChecks()
{
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, void (*run)()) { registry().push_back(TestCase{name, run}); }
};

inline void checkTrue(bool ok, const char* expr, const char* file, int line)
{
    if (ok) {
        return;
    }
    std::printf("%s:%d: CHECK(%s) failed\n", file, line, expr);
    ++failedChecks();
}

template <typename A, typename B>
void checkEqual(const A& a, const B& b, const char* exprA, const char* exprB, const char* file, int line)
{
    if (a == b) {
        return;
    }
    std::ostringstream left;
    std::ostringstream right;
    left << a;
    right << b;
    std::printf("%s:%d: CHECK_EQ(%s, %s) failed: %s != %s\n", file, line, exprA, exprB,
                left.str().c_str(), right.str().c_str());
    ++failedChecks();
}

// Runs tests whose name contains filter (empty runs all) and returns the exit code.
inline int runTests(const std::string& filter, bool listOnly)
{
    int ran = 0;
    int failedTests = 0;
    for (const TestCase& test : registry()) {
        if (!filter.empty() && std::string(test.name).find(filter) == std::string::npos) {
            continue;
        }
        if (listOnly) {
            std::printf("%s\n", test.name);
            continue;
        }
        const int before = failedChecks();
        test.run();
        ++ran;
        if (failedChecks() != before) {
            ++failedTests;
            std::printf("FAILED: %s\n", test.name);
        }
    }
    if (listOnly) {
        return 0;
    }
    std::printf("%d tests run, %d failed, %d failed checks\n", ran, failedTests, failedChecks());
    return failedChecks() == 0 ? 0 : 1;
}

}  // namespace telaviva_test

#define TEST(name)                                                                        \
    static void telaviva_test_##name();                                                   \
    static const ::telaviva_test::Registrar telaviva_registrar_##name(#name,              \
                                                                     &telaviva_test_##name); \
    static void telaviva_test_##name()

#define CHECK(expr) ::telaviva_test::checkTrue((expr), #expr, __FILE__, __LINE__)

#define CHECK_EQ(a, b) ::telaviva_test::checkEqual((a), (b), #a, #b, __FILE__, __LINE__)
