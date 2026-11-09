#include "Ue4ssGame.hpp"
#include "../SafeCall.hpp"
#include <cbmp/Log.hpp>
#include <Helpers/String.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Rotator.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/World.hpp>
#include <format>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

#define CBMP_T(s) FromCharTypePtr<TCHAR>(STR(s))

static UObject* ReadObjectProp(UObject* owner, const TCHAR* prop) {
    if (!owner) return nullptr;
    auto** v = owner->GetValuePtrByPropertyNameInChain<UObject*>(prop);
    return v ? *v : nullptr;
}

UObject* Ue4ssGame::FindPlayerController() {
    UObject* pc = SafeCall("FindPlayerController", [] { return UObjectGlobals::FindFirstOf(STR("PlayerController")); });
    if (pc && !m_loggedPc) {
        m_loggedPc = true;
        SafeCall("LogPcClass", [&] {
            cbmp::Log().Write(cbmp::Level::Info, "PlayerController encontrado: classe " + to_string(pc->GetClassPrivate()->GetName()));
        });
    }
    return pc;
}

std::optional<cbmp::PlayerHandle> Ue4ssGame::GetLocalPlayer() {
    UObject* pc = FindPlayerController();
    if (!pc) return std::nullopt;
    return SafeCall("GetLocalPlayer", [&]() -> std::optional<cbmp::PlayerHandle> {
        UObject* pawn = ReadObjectProp(pc, CBMP_T("Pawn"));
        if (!pawn) pawn = ReadObjectProp(pc, CBMP_T("AcknowledgedPawn"));
        if (!pawn) return std::nullopt;
        return cbmp::PlayerHandle{pawn};
    });
}

std::optional<cbmp::Transform> Ue4ssGame::GetTransform(cbmp::PlayerHandle p) {
    return SafeCall("GetTransform", [&]() -> std::optional<cbmp::Transform> {
        auto* actor = static_cast<UObject*>(p.ptr);
        UObject* root = ReadObjectProp(actor, CBMP_T("RootComponent"));
        if (!root) return std::nullopt;
        auto* loc = root->GetValuePtrByPropertyNameInChain<FVector>(CBMP_T("RelativeLocation"));
        auto* rot = root->GetValuePtrByPropertyNameInChain<FRotator>(CBMP_T("RelativeRotation"));
        if (!loc || !rot) return std::nullopt;
        return cbmp::Transform{{loc->X(), loc->Y(), loc->Z()}, {rot->GetPitch(), rot->GetYaw(), rot->GetRoll()}};
    });
}

std::optional<cbmp::PlayerHandle> Ue4ssGame::SpawnClone(const cbmp::Transform& at) {
    auto player = GetLocalPlayer();
    if (!player) return std::nullopt;
    return SafeCall("SpawnClone", [&]() -> std::optional<cbmp::PlayerHandle> {
        auto* pawn = static_cast<UObject*>(player->ptr);
        // Mundo: Engine -> GameViewport -> World (todos UPROPERTY).
        UObject* engine = UObjectGlobals::FindFirstOf(STR("GameEngine"));
        UObject* viewport = ReadObjectProp(engine, CBMP_T("GameViewport"));
        auto* world = static_cast<UWorld*>(ReadObjectProp(viewport, CBMP_T("World")));
        if (!world) {
            cbmp::Log().Write(cbmp::Level::Warn, "SpawnClone: UWorld não encontrado via GameEngine.GameViewport.World");
            return std::nullopt;
        }
        UClass* cls = pawn->GetClassPrivate();
        cbmp::Log().Write(cbmp::Level::Info, "SpawnClone: spawnando classe " + to_string(cls->GetFullName()));
        FVector loc{};
        loc.SetX(at.pos.x); loc.SetY(at.pos.y); loc.SetZ(at.pos.z);
        FRotator rot{};
        rot.SetPitch(0.0); rot.SetYaw(at.rot.yaw); rot.SetRoll(0.0);
        AActor* actor = world->SpawnActor(cls, &loc, &rot);
        if (!actor) return std::nullopt;
        return cbmp::PlayerHandle{actor};
    });
}

void Ue4ssGame::Destroy(cbmp::PlayerHandle p) {
    SafeCall("Destroy", [&]() {
        auto* actor = static_cast<UObject*>(p.ptr);
        UFunction* fn = actor->GetFunctionByNameInChain(CBMP_T("K2_DestroyActor"));
        if (!fn) throw cbmp::GameError("K2_DestroyActor não encontrada");
        actor->ProcessEvent(fn, nullptr);
    });
}

std::string Ue4ssGame::Describe(cbmp::PlayerHandle p) {
    return SafeCall("Describe", [&]() -> std::string {
        auto* o = static_cast<UObject*>(p.ptr);
        return std::format("{} | {}", to_string(o->GetClassPrivate()->GetName()), to_string(o->GetFullName()));
    });
}

std::string Ue4ssGame::Diagnose() {
    return SafeCall("Diagnose", [&]() -> std::string {
        std::vector<UObject*> all;
        UObjectGlobals::FindAllOf(STR("PlayerController"), all);
        UObject* pc = UObjectGlobals::FindFirstOf(STR("PlayerController"));
        if (!pc) return std::format("nenhum PlayerController (FindAllOf: {})", all.size());
        auto name = [](UObject* o) { return o ? to_string(o->GetFullName()) : std::string("null"); };
        return std::format("PlayerController: {} (FindAllOf: {}) | Pawn: {} | AcknowledgedPawn: {}", name(pc), all.size(),
                           name(ReadObjectProp(pc, CBMP_T("Pawn"))), name(ReadObjectProp(pc, CBMP_T("AcknowledgedPawn"))));
    });
}
