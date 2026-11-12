#pragma once
// Casca do mod no UE4SS. Inerte sem o launcher (TryArm falhou: nada é aberto, gravado, criado ou registrado). Armado:
// log por processo, portão de dois processos, crash dump, cb-mp.json e, em on_unreal_init, GNS + Session no tick do
// motor (game thread). Toda a lógica está no núcleo (Session); aqui só a ligação.
#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks/GlobalCallbackId.hpp>

#include <cbmp/client/ModConfig.hpp>

#include <memory>
#include <mutex>
#include <optional>

namespace cbmp::client {
struct Armed;
class GnsTransport;
class Session;
class Ue4ssGame;
class WinConnectFs;
class WinProcessGate;

class CbMpMod final : public RC::CppUserModBase {
public:
    CbMpMod();
    ~CbMpMod() override;
    auto on_unreal_init() -> void override;

private:
    // Dono compartilhado com o callback do tick: o destrutor zera `owner` sob a trava, então um tick atrasado (ou um
    // que já estava rodando) nunca usa um CbMpMod destruído.
    struct TickGate {
        std::mutex mutex;
        CbMpMod* owner = nullptr;
    };

    void OnGameTick();       // game thread, sob TickGate::mutex
    void OnTickException();  // chamado no catch(...) do tick; pode lançar (o chamador engole)

    std::unique_ptr<Armed> m_armed;  // nulo = inerte
    ModConfig m_config;
    std::unique_ptr<WinProcessGate> m_gate;
    bool m_gnsStarted = false;
    std::unique_ptr<Ue4ssGame> m_game;
    std::unique_ptr<GnsTransport> m_transport;
    std::unique_ptr<WinConnectFs> m_connectFs;
    std::unique_ptr<Session> m_session;
    std::shared_ptr<TickGate> m_tickGate;
    std::optional<RC::Unreal::Hook::GlobalCallbackId> m_tickHookId;

    // Só do game thread:
    bool m_firstTickLogged = false;
    bool m_tickExceptionSeen = false;
    long long m_tickExceptionsPending = 0;
    double m_lastTickExceptionLog = 0.0;
};
}  // namespace cbmp::client
