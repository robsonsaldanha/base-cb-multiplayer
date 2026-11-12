// WinProcessGate com objetos nomeados reais (nomes únicos por teste: Local\cbmp-test-<pid>-<n>).
// O mutex do Windows é reentrante por thread: o "outro processo" é simulado com outra thread.
#include <catch2/catch_test_macros.hpp>
#include <WinProcessGate.hpp>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <thread>

using namespace cbmp::client;

namespace {
struct Names {
    std::wstring epoch, owner;
};

Names UniqueNames() {
    static std::atomic<int> n{0};
    const int i = n++;
    const auto pid = GetCurrentProcessId();
    return {std::format(L"Local\\cbmp-test-{}-{}-epoch", pid, i), std::format(L"Local\\cbmp-test-{}-{}-owner", pid, i)};
}
}  // namespace

TEST_CASE("client: WinProcessGate nomes da spec") {
    CHECK(std::wstring(WinProcessGate::kEpochName) == L"Local\\cbmp-mod-epoch-v1");
    CHECK(std::wstring(WinProcessGate::kOwnerName) == L"Local\\cbmp-mod-owner-v1");
}

TEST_CASE("client: WinProcessGate epoca e dono entre dois portoes") {
    const auto names = UniqueNames();
    WinProcessGate first(names.epoch, names.owner);
    REQUIRE(first.Ok());
    CHECK(first.Epoch() == 1);
    CHECK(first.Latest() == 1);
    CHECK(first.IsCurrent());
    CHECK(first.TryOwn());
    CHECK(first.TryOwn());  // já é dono

    bool secondOk = false, secondCurrent = false, secondOwns = true, firstCurrentAfter = true;
    uint64_t secondEpoch = 0, firstLatestAfter = 0;
    std::thread([&] {
        WinProcessGate second(names.epoch, names.owner);
        secondOk = second.Ok();
        secondEpoch = second.Epoch();
        secondCurrent = second.IsCurrent();
        firstCurrentAfter = first.IsCurrent();
        firstLatestAfter = first.Latest();
        secondOwns = second.TryOwn();
    }).join();

    CHECK(secondOk);
    CHECK(secondEpoch == 2);
    CHECK(secondCurrent);
    CHECK_FALSE(firstCurrentAfter);
    CHECK(firstLatestAfter == 2);
    CHECK_FALSE(secondOwns);
    CHECK(first.Epoch() == 1);
    CHECK_FALSE(first.IsCurrent());  // o contador nunca desce
}

TEST_CASE("client: WinProcessGate recomeca em 1 depois de destruir todos") {
    const auto names = UniqueNames();
    {
        WinProcessGate a(names.epoch, names.owner);
        WinProcessGate b(names.epoch, names.owner);
        CHECK(a.Epoch() == 1);
        CHECK(b.Epoch() == 2);
    }
    WinProcessGate c(names.epoch, names.owner);
    REQUIRE(c.Ok());
    CHECK(c.Epoch() == 1);
    CHECK(c.IsCurrent());
}

TEST_CASE("client: WinProcessGate libera o dono no destrutor") {
    const auto names = UniqueNames();
    // `keeper` segura um handle dos mesmos objetos durante todo o teste: sem ele, destruir `a` (único handle) tiraria o
    // nome do kernel e `b` criaria um mutex novo, passando com ou sem ReleaseMutex. Ele nunca chama TryOwn.
    WinProcessGate keeper(names.epoch, names.owner);
    REQUIRE(keeper.Ok());
    {
        WinProcessGate a(names.epoch, names.owner);
        REQUIRE(a.TryOwn());
    }
    bool owns = false;
    std::thread([&] {
        WinProcessGate b(names.epoch, names.owner);
        owns = b.TryOwn();
    }).join();
    CHECK(owns);
}

TEST_CASE("client: WinProcessGate aceita mutex abandonado") {
    const auto names = UniqueNames();
    std::unique_ptr<WinProcessGate> dead;
    bool deadOwned = false;
    // A thread toma o mutex e termina sem soltar: o mutex fica abandonado.
    std::thread([&] {
        dead = std::make_unique<WinProcessGate>(names.epoch, names.owner);
        deadOwned = dead->TryOwn();
    }).join();
    REQUIRE(deadOwned);

    WinProcessGate heir(names.epoch, names.owner);
    CHECK(heir.Epoch() == 2);
    CHECK(heir.TryOwn());
    dead.reset();  // destrutor em outra thread: não pode soltar o mutex que agora é do herdeiro

    bool otherOwns = true;
    std::thread([&] {
        WinProcessGate other(names.epoch, names.owner);
        otherOwns = other.TryOwn();
    }).join();
    CHECK_FALSE(otherOwns);
}

TEST_CASE("client: WinProcessGate nome invalido nao e Ok e nunca e dono") {
    // Barra invertida extra depois de "Local\" é inválida para objetos nomeados.
    WinProcessGate g(L"Local\\a\\b\\c", L"Local\\a\\b\\d");
    CHECK_FALSE(g.Ok());
    CHECK(g.Epoch() == 1);
    CHECK(g.IsCurrent());
    CHECK_FALSE(g.TryOwn());
}
