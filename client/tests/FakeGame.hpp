#pragma once
#include <cbmp/IGame.hpp>
#include <string>
#include <vector>

struct FakeGame : cbmp::IGame {
    bool hasPlayer = false;
    bool throwOnGetPlayer = false;
    cbmp::Transform playerTransform{};
    int spawnCalls = 0;
    int destroyCalls = 0;
    int getPlayerCalls = 0;
    bool throwOnDiagnose = false;
    static constexpr const char* kDiag = "FakeDiag: nenhum PlayerController";
    std::vector<cbmp::Transform> spawnedAt;
    int playerTag = 1;
    int cloneTag = 2;

    std::optional<cbmp::PlayerHandle> GetLocalPlayer() override {
        ++getPlayerCalls;
        if (throwOnGetPlayer) throw cbmp::GameError("falha simulada");
        if (!hasPlayer) return std::nullopt;
        return cbmp::PlayerHandle{&playerTag};
    }
    std::optional<cbmp::Transform> GetTransform(cbmp::PlayerHandle) override { return playerTransform; }
    std::optional<cbmp::PlayerHandle> SpawnClone(const cbmp::Transform& t) override {
        ++spawnCalls; spawnedAt.push_back(t);
        return cbmp::PlayerHandle{&cloneTag};
    }
    void Destroy(cbmp::PlayerHandle) override { ++destroyCalls; }
    std::string Describe(cbmp::PlayerHandle) override { return "FakePawn /Game/Fake.FakePawn"; }
    std::string Diagnose() override {
        if (throwOnDiagnose) throw cbmp::GameError("diag simulado quebrou");
        return kDiag;
    }
};
