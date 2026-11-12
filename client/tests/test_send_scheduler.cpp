#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/SendScheduler.hpp>

using namespace cbmp::client;

TEST_CASE("client: SendScheduler a 20 Hz") {
    SendScheduler s;
    s.SetRate(20);
    REQUIRE(s.Due(0.0));
    REQUIRE_FALSE(s.Due(0.0));
    REQUIRE_FALSE(s.Due(0.049));
    REQUIRE(s.Due(0.05));
    REQUIRE_FALSE(s.Due(0.099));
    REQUIRE(s.Due(0.10));
}

TEST_CASE("client: SendScheduler tick atrasado nao gera rajada") {
    SendScheduler s;
    s.SetRate(20);
    REQUIRE(s.Due(0.0));
    REQUIRE(s.Due(1.0));  // atrasou 1 s: um só verdadeiro
    REQUIRE_FALSE(s.Due(1.0));
    REQUIRE_FALSE(s.Due(1.001));
    REQUIRE_FALSE(s.Due(1.049));
    REQUIRE(s.Due(1.05));  // o próximo só 0,05 s depois
    REQUIRE_FALSE(s.Due(1.06));
}

TEST_CASE("client: SendScheduler mantem 20 Hz com quadros de 16 ms") {
    SendScheduler s;
    s.SetRate(20);
    int n = 0;
    for (int i = 0; i < 625; ++i) {  // 10 s
        if (s.Due(i * 0.016)) ++n;
    }
    REQUIRE(n >= 199);
    REQUIRE(n <= 201);
}

TEST_CASE("client: SendScheduler SetRate 60") {
    SendScheduler s;
    s.SetRate(60);
    REQUIRE(s.Due(1.0));
    REQUIRE_FALSE(s.Due(1.0 + 1.0 / 60 - 0.001));
    REQUIRE(s.Due(1.0 + 1.0 / 60));
    int n = 0;
    for (int i = 0; i < 1000; ++i) {  // 10 s em passos de 10 ms
        if (s.Due(2.0 + i * 0.010)) ++n;
    }
    REQUIRE(n >= 599);
    REQUIRE(n <= 601);

    s.SetRate(0);  // ignorado
    s.SetRate(-5);
    REQUIRE(s.Due(100.0));
    REQUIRE_FALSE(s.Due(100.0 + 1.0 / 60 - 0.001));
}

TEST_CASE("client: SendScheduler Reset deixa o proximo Due verdadeiro") {
    SendScheduler s;
    s.SetRate(20);
    REQUIRE(s.Due(5.0));
    REQUIRE_FALSE(s.Due(5.01));
    s.Reset();
    REQUIRE(s.Due(5.02));
    REQUIRE_FALSE(s.Due(5.03));
    REQUIRE(s.Due(5.07));
}
