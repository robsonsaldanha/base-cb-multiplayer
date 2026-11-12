#pragma once
// Jogo roteirizado do cbmp-headless: mundo "Overland" (worldId 1), pawn 1 em `start` desde o início; depois de
// `walkAfter` s anda `walkCm` em +X a `speedCmS` e para. Bonecos ficam em memória com o caminho de cada um.
#include <cbmp/client/IGame.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cbmp::headless {
struct PathPoint {
    double t = 0;  // segundos Unix
    cbmp::client::Vec3 pos;
};

struct PuppetRecord {
    uint32_t id = 0;
    double createdAt = 0;
    std::optional<double> destroyedAt;
    std::vector<PathPoint> path;
};

struct ScriptOptions {
    cbmp::client::Vec3 start;
    double walkAfterSec = 6;
    double walkCm = 2000;
    double speedCmS = 300;
};

class ScriptedGame final : public cbmp::client::IGame {
public:
    explicit ScriptedGame(ScriptOptions opt);

    // Avança o roteiro até `elapsedSec` (desde o início) e grava a posição local no instante Unix `unixSec`.
    void Advance(double elapsedSec, double unixSec);

    cbmp::client::WorldSample SampleWorld() override;
    std::optional<cbmp::client::LocalState> ReadLocal() override;
    bool TeleportLocal(const cbmp::client::Vec3& pos) override;
    std::optional<cbmp::client::PuppetId> SpawnPuppet(const cbmp::client::PuppetPose& at) override;
    cbmp::client::PuppetResult MovePuppet(cbmp::client::PuppetId id, const cbmp::client::PuppetPose& to) override;
    void DestroyPuppet(cbmp::client::PuppetId id) override;
    void ForgetAllPuppets() override;
    bool PlayerSingletonIntact() override;
    bool ShowStatus(std::string_view utf8) override;
    std::string Diagnose() override;

    const std::vector<PathPoint>& LocalPath() const { return m_local; }
    const std::map<uint32_t, PuppetRecord>& Puppets() const { return m_puppets; }
    const std::vector<std::string>& Statuses() const { return m_statuses; }

private:
    ScriptOptions m_opt;
    cbmp::client::Vec3 m_pos;
    cbmp::client::Vec3 m_vel;
    double m_walked = 0;
    double m_lastElapsed = 0;
    double m_unix = 0;
    std::vector<PathPoint> m_local;
    std::map<uint32_t, PuppetRecord> m_puppets;
    uint32_t m_nextPuppet = 1;
    std::vector<std::string> m_statuses;
};
}  // namespace cbmp::headless
