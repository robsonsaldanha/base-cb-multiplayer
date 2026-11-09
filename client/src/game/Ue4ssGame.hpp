#pragma once
#include <cbmp/IGame.hpp>

namespace RC::Unreal { class UObject; }

// Única classe que conhece UE4SS/Unreal. Chamar só no game thread.
class Ue4ssGame final : public cbmp::IGame {
public:
    std::optional<cbmp::PlayerHandle> GetLocalPlayer() override;
    std::optional<cbmp::Transform> GetTransform(cbmp::PlayerHandle p) override;
    std::optional<cbmp::PlayerHandle> SpawnClone(const cbmp::Transform& at) override;
    void Destroy(cbmp::PlayerHandle p) override;
    std::string Describe(cbmp::PlayerHandle p) override;
    std::string Diagnose() override;

private:
    RC::Unreal::UObject* FindPlayerController();
    bool m_loggedPc = false;
};
