#pragma once
#include <Mod/CppUserModBase.hpp>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <Unreal/Hooks/GlobalCallbackId.hpp>

namespace cbmp { class SpikeController; }
class Ue4ssGame;

class CbMpMod final : public RC::CppUserModBase {
public:
    CbMpMod();
    ~CbMpMod() override;
    auto on_unreal_init() -> void override;

    static std::filesystem::path ModDir(); // pasta ...\Mods\cb-mp\ (pai de dlls\)

private:
    void OnGameTick();       // game thread
    void OnTickException();  // chamado no catch(...) do tick; pode lançar (o chamador engole)

    std::optional<RC::Unreal::Hook::GlobalCallbackId> m_tickHookId;
    std::unique_ptr<Ue4ssGame> m_game;
    std::unique_ptr<cbmp::SpikeController> m_controller;
    std::atomic<int> m_pendingToggles{0}; // F8 vem de outra thread; consumido no tick
    // Estado só do game thread:
    bool m_firstTickLogged = false;
    bool m_tickExceptionSeen = false;
    long long m_tickExceptionsPending = 0;
    double m_lastTickExceptionLog = 0.0;
};
