#pragma once

// Minimal dependency-free test harness.

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace test {

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) { registry().push_back({std::move(name), std::move(fn)}); }
};

struct Failure {
    std::string message;
};

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

#define TEST_CONCAT_(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_(a, b)

#define TEST(name)                                                                           \
    static void TEST_CONCAT(test_fn_, name)();                                               \
    static test::Registrar TEST_CONCAT(test_reg_, name)(#name, TEST_CONCAT(test_fn_, name)); \
    static void TEST_CONCAT(test_fn_, name)()

#define CHECK(cond)                                                                                             \
    do {                                                                                                        \
        if (!(cond))                                                                                            \
            throw test::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #cond ")"}; \
    } while (0)

#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        if (!((a) == (b)))                                                               \
            throw test::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                ": CHECK_EQ(" #a ", " #b ")"};                           \
    } while (0)

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
