#pragma once
// Liga RemoteStore + Interp ao IGame: cria, move e destrói os bonecos dos outros jogadores. Só entidades conhecidas a
// até 400 m do jogador local, as mais próximas primeiro, no máximo `maxPuppets` (0..64); o próprio jogador nunca
// vira boneco. Falhas do jogo têm espera e desistência; erros demais ou o singleton do jogador trocado desligam os
// bonecos na sessão (a conexão segue).
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <cbmp/client/IGame.hpp>
#include <cbmp/client/Interp.hpp>
#include <cbmp/client/Log.hpp>
#include <cbmp/client/RemoteStore.hpp>

namespace cbmp::client {
struct PuppetStats {
    int alive = 0;
    uint64_t created = 0, destroyed = 0, recreated = 0, givenUp = 0, gameErrors = 0;
    bool disabled = false;
    std::string disabledWhy;  // "singleton" ou "erros"
};

class PuppetDirector {
public:
    PuppetDirector(IGame& game, const RemoteStore& store, Interp& interp, LogFn log, int maxPuppets);
    void SetSelf(uint16_t playerId);
    void Update(double now, const Vec3& localPos);  // chamar só em Playing
    void OnEntityLeave(uint16_t id);
    void ReleaseAll(bool worldChanged);  // true → ForgetAllPuppets; false → DestroyPuppet de cada um
    const PuppetStats& Stats() const;

private:
    struct Slot {
        std::optional<PuppetId> puppet;
        int spawnFailures = 0;         // SpawnPuppet nulos seguidos
        double retryAt = 0;            // próxima tentativa de SpawnPuppet permitida
        bool recreatePending = false;  // próximo SpawnPuppet com sucesso é recriação
        bool givenUp = false;
        int moveErrors = 0;            // erros seguidos em MovePuppet
        std::deque<double> recreates;  // instantes das recriações agendadas (último minuto)
    };

    void Info(std::string_view msg) const;
    void Warn(std::string_view msg) const;
    bool CountError(double now);  // verdadeiro = limite atingido (pede o desligamento)
    void Destroy(uint16_t id, Slot& slot, std::string_view why);
    void HandleGone(uint16_t id, Slot& slot, double now);
    void GiveUp(uint16_t id, Slot& slot);
    void Spawn(uint16_t id, Slot& slot, const PuppetPose& pose, double now);
    void Move(uint16_t id, Slot& slot, const PuppetPose& pose, double now);
    void Disable(std::string_view why);
    void RefreshAlive();

    IGame& m_game;
    const RemoteStore& m_store;
    Interp& m_interp;
    LogFn m_log;
    int m_maxPuppets;
    std::optional<uint16_t> m_self;
    std::map<uint16_t, Slot> m_slots;
    std::deque<double> m_errors;  // instantes dos erros de jogo (últimos 10 s)
    bool m_singletonLogged = false;
    std::optional<std::string> m_disableRequest;
    PuppetStats m_stats;
};
}  // namespace cbmp::client
