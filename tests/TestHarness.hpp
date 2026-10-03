#pragma once

/// @file
/// Minimal dependency-free test harness.
///
/// Every test executable includes it, declares cases with TEST() and ends with
/// `int main() { return test::runAll(); }`:
///
/// @code
/// TEST(empty_names_are_rejected) {
///     CHECK_THROWS_AS(rules::name(""), ValidationError);
///     CHECK_EQ(rules::name(" Dune "), "Dune");
/// }
/// @endcode
///
/// A failed check throws test::Failure, which ends that case only; runAll()
/// reports `[ OK ]` / `[FAIL]` per case and returns non-zero if any failed, so
/// CTest sees the failure.
/// @ingroup tests

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace test {

/// One registered test case.
struct Case {
    std::string name;          ///< The name given to TEST().
    std::function<void()> fn;  ///< Its body.
};

/// Every case of this executable, in declaration order.
inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

/// Adds a case to registry() during static initialization (used by TEST()).
struct Registrar {
    /// @param name  The case's name.
    /// @param fn    Its body.
    Registrar(std::string name, std::function<void()> fn) { registry().push_back({std::move(name), std::move(fn)}); }
};

/// Thrown by a failed check; ends the current case.
struct Failure {
    std::string message;  ///< `file:line: CHECK(...)`, or a test's own explanation.
};

/// Runs every registered case, printing one line per case and a summary.
/// @return 0 if all passed, 1 otherwise (the executable's exit code).
inline int runAll() {
    int failed = 0;
    for (const auto& c : registry()) {
        try {
            c.fn();
            std::cout << "[ OK ] " << c.name << "\n";
        } catch (const Failure& f) {
            ++failed;
            std::cout << "[FAIL] " << c.name << "\n       " << f.message << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[FAIL] " << c.name << "\n       unexpected exception: " << e.what() << "\n";
        }
    }
    std::cout << "\n"
              << registry().size() - static_cast<std::size_t>(failed) << "/" << registry().size() << " passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace test

/// @cond INTERNAL
#define TEST_CONCAT_(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_(a, b)
/// @endcond

/// Declares a test case; the block that follows is its body. The name says
/// what the case proves, in words: `TEST(idle_connections_are_closed) { ... }`.
#define TEST(name)                                                                           \
    static void TEST_CONCAT(test_fn_, name)();                                               \
    static test::Registrar TEST_CONCAT(test_reg_, name)(#name, TEST_CONCAT(test_fn_, name)); \
    static void TEST_CONCAT(test_fn_, name)()

/// Fails the case unless `cond` is true.
#define CHECK(cond)                                                                                             \
    do {                                                                                                        \
        if (!(cond))                                                                                            \
            throw test::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #cond ")"}; \
    } while (0)

/// Fails the case unless `a == b`.
#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        if (!((a) == (b)))                                                               \
            throw test::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                ": CHECK_EQ(" #a ", " #b ")"};                           \
    } while (0)

/// Fails the case unless evaluating `expr` throws `type` (or a subclass).
#define CHECK_THROWS_AS(expr, type)                                                      \
    do {                                                                                 \
        bool caught_ = false;                                                            \
        try {                                                                            \
            (void)(expr);                                                                \
        } catch (const type&) {                                                          \
            caught_ = true;                                                              \
        }                                                                                \
        if (!caught_)                                                                    \
            throw test::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                ": expected " #type " from " #expr};                     \
    } while (0)
