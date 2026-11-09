#pragma once
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace cbmp_test {
struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& Registry() { static std::vector<TestCase> r; return r; }
inline int& Failures() { static int f = 0; return f; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { Registry().push_back({n, std::move(f)}); } };
}

#define CBMP_CAT2(a, b) a##b
#define CBMP_CAT(a, b) CBMP_CAT2(a, b)
#define TEST(name) \
    static void name(); \
    static ::cbmp_test::Registrar CBMP_CAT(reg_, name){#name, &name}; \
    static void name()
#define CHECK(expr) \
    do { if (!(expr)) { ++::cbmp_test::Failures(); \
        std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK falhou: " #expr "\n"; } } while (0)
#define CHECK_EQ(a, b) \
    do { auto _va = (a); auto _vb = (b); if (!(_va == _vb)) { ++::cbmp_test::Failures(); \
        std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ falhou: " #a " == " #b \
                  << "\n  esquerda: " << _va << "\n  direita:  " << _vb << "\n"; } } while (0)
