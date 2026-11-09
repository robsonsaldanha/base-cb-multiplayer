#include "TestFramework.hpp"

int main() {
    for (auto& t : ::cbmp_test::Registry()) {
        int before = ::cbmp_test::Failures();
        t.fn();
        std::cout << (::cbmp_test::Failures() == before ? "[ OK ] " : "[FAIL] ") << t.name << "\n";
    }
    std::cout << ::cbmp_test::Registry().size() << " testes, " << ::cbmp_test::Failures() << " falhas\n";
    return ::cbmp_test::Failures() == 0 ? 0 : 1;
}
