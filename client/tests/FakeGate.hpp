#pragma once
// Portas falsas dos dois processos (IProcessGate) e do resgate do connect.json (IConnectFs) para os testes.
#include <cbmp/client/ConnectFile.hpp>
#include <cbmp/client/ProcessGate.hpp>
#include <cstdint>
#include <deque>
#include <utility>

namespace cbmp::client {
struct FakeGate : IProcessGate {
    uint64_t epoch = 1, latest = 1;
    bool ownOk = true;
    int ownCalls = 0;

    uint64_t Epoch() const override { return epoch; }
    uint64_t Latest() override { return latest; }
    bool IsCurrent() override { return Latest() == Epoch(); }
    bool TryOwn() override {
        ++ownCalls;
        return ownOk;
    }
};

struct FakeConnectFs : IConnectFs {
    bool exists = true;
    std::deque<ClaimResult> results;  // sem resultados na fila → Missing
    int claimCalls = 0;

    bool Exists() override { return exists; }
    ClaimResult ClaimAndDelete() override {
        ++claimCalls;
        if (results.empty()) return {};
        ClaimResult r = std::move(results.front());
        results.pop_front();
        return r;
    }
};
}  // namespace cbmp::client
