#pragma once
// IGame sobre o UE4SS: casca fina, toda chamada ao jogo passa por Reflect (por nome). Só no game thread (o construtor
// e o destrutor não tocam o jogo). Cada método público roda inteiro dentro de SafeCall (falha → GameError).
#include <cbmp/client/GameGlue.hpp>
#include <cbmp/client/IGame.hpp>
#include <cbmp/client/ModConfig.hpp>

#include "Reflect.hpp"
#include <Unreal/FWeakObjectPtr.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace RC::Unreal {
class UObject;
class UWorld;
}  // namespace RC::Unreal

namespace cbmp::client {
class Ue4ssGame final : public IGame {
public:
    explicit Ue4ssGame(ModConfig cfg);

    WorldSample SampleWorld() override;
    std::optional<LocalState> ReadLocal() override;
    bool TeleportLocal(const Vec3& pos) override;
    std::optional<PuppetId> SpawnPuppet(const PuppetPose& at) override;
    PuppetResult MovePuppet(PuppetId id, const PuppetPose& to) override;
    void DestroyPuppet(PuppetId id) override;
    void ForgetAllPuppets() override;
    bool PlayerSingletonIntact() override;
    bool ShowStatus(std::string_view utf8) override;
    std::string Diagnose() override;

private:
    RC::Unreal::UWorld* CurrentWorld();
    RC::Unreal::UObject* PlayerController(RC::Unreal::UWorld* world);
    RC::Unreal::UObject* LocalPawn(RC::Unreal::UWorld* world);
    RC::Unreal::UObject* ObjectProp(RC::Unreal::UObject* owner, const RC::Unreal::TCHAR* name, const char* label);
    RC::Unreal::UObject* UiManager();
    bool FrontendHint();
    RC::Unreal::UObject* Component(RC::Unreal::UObject* actor, const RC::Unreal::TCHAR* className, const char* label);
    void Neutralize(RC::Unreal::UObject* actor);
    void Step(const std::string& what, RC::Unreal::UObject* target, const RC::Unreal::TCHAR* function,
              std::initializer_list<Arg> args);
    void Animate(RC::Unreal::UObject* actor, const PuppetPose& to);
    std::string DiagnoseUnsafe();
    void InfoOnce(const std::string& key, const std::string& msg);
    void WarnOnce(const std::string& key, const std::string& msg);

    ModConfig m_cfg;
    RC::Unreal::FWeakObjectPtr m_engine;      // GameEngine
    RC::Unreal::FWeakObjectPtr m_controller;  // PlayerController do mundo atual
    RC::Unreal::FWeakObjectPtr m_controllerWorld;  // mundo em que m_controller foi achado
    RC::Unreal::FWeakObjectPtr m_uiManager;
    std::chrono::steady_clock::time_point m_nextEngineSearch{};
    std::chrono::steady_clock::time_point m_nextControllerSearch{};
    uint64_t m_lastPawnKey = 0;               // para registrar Diagnose quando o pawn local troca
    PuppetTable<RC::Unreal::FWeakObjectPtr> m_puppets;
    FallbackLadder m_teleport{2};  // K2_TeleportTo → K2_SetActorLocation
    FallbackLadder m_move{3};      // K2_SetActorLocationAndRotation → K2_TeleportTo → RootComponent
    bool m_animDisabled = false;
    OnceSet m_once;
};
}  // namespace cbmp::client
