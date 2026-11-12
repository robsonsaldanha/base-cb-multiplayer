#pragma once
// Jogo falso para os testes: implementa o IGame novo, conta as chamadas e confere que nenhum valor não finito chega
// ao jogo (REQUIRE em todo método que recebe uma pose ou posição).
#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/IGame.hpp>

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cbmp::client {
struct FakeGame : IGame {
    WorldSample world{1, "Overland", 10, true, false};
    std::optional<LocalState> local;
    std::map<uint32_t, PuppetPose> puppets;  // bonecos vivos no "jogo"
    uint32_t nextId = 1;
    int spawnCalls = 0, moveCalls = 0, destroyCalls = 0, forgetAllCalls = 0, singletonCalls = 0;
    int spawnFailures = 0;      // próximos N SpawnPuppet → nullopt
    std::set<uint32_t> gone;    // MovePuppet desses ids → Gone (o "jogo" destruiu o ator)
    std::set<uint32_t> failMove;  // MovePuppet desses ids → Failed
    bool throwOnMove = false, throwOnSpawn = false, throwOnSample = false;
    bool singletonIntact = true, statusOk = true;
    std::vector<std::string> statuses;
    std::vector<Vec3> teleports;
    bool teleportOk = true;
    std::vector<uint32_t> destroyed;  // ids passados a DestroyPuppet, em ordem

    static void RequireFinite(const Vec3& v) {
        REQUIRE(std::isfinite(v.x));
        REQUIRE(std::isfinite(v.y));
        REQUIRE(std::isfinite(v.z));
    }
    static void RequireFinite(const PuppetPose& p) {
        RequireFinite(p.pos);
        RequireFinite(p.vel);
        REQUIRE(std::isfinite(p.yawDeg));
    }

    WorldSample SampleWorld() override {
        if (throwOnSample) throw GameError("SampleWorld falhou (teste)");
        return world;
    }
    std::optional<LocalState> ReadLocal() override {
        if (throwOnSample) throw GameError("ReadLocal falhou (teste)");
        return local;
    }
    bool TeleportLocal(const Vec3& pos) override {
        RequireFinite(pos);
        teleports.push_back(pos);
        if (teleportOk && local) local->pos = pos;
        return teleportOk;
    }
    std::optional<PuppetId> SpawnPuppet(const PuppetPose& at) override {
        RequireFinite(at);
        ++spawnCalls;
        if (throwOnSpawn) throw GameError("SpawnPuppet falhou (teste)");
        if (spawnFailures > 0) {
            --spawnFailures;
            return std::nullopt;
        }
        const uint32_t id = nextId++;
        puppets[id] = at;
        return PuppetId{id};
    }
    PuppetResult MovePuppet(PuppetId id, const PuppetPose& to) override {
        RequireFinite(to);
        ++moveCalls;
        if (throwOnMove) throw GameError("MovePuppet falhou (teste)");
        if (gone.contains(id.v) || !puppets.contains(id.v)) {
            puppets.erase(id.v);
            return PuppetResult::Gone;
        }
        if (failMove.contains(id.v)) return PuppetResult::Failed;
        puppets[id.v] = to;
        return PuppetResult::Ok;
    }
    void DestroyPuppet(PuppetId id) override {
        ++destroyCalls;
        destroyed.push_back(id.v);
        puppets.erase(id.v);
    }
    void ForgetAllPuppets() override {
        ++forgetAllCalls;
        puppets.clear();
    }
    bool PlayerSingletonIntact() override {
        ++singletonCalls;
        return singletonIntact;
    }
    bool ShowStatus(std::string_view utf8) override {
        statuses.emplace_back(utf8);
        return statusOk;
    }
    std::string Diagnose() override { return "FakeGame"; }
};
}  // namespace cbmp::client
