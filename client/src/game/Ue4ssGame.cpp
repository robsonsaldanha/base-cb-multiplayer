#include "Ue4ssGame.hpp"
#include "../SafeCall.hpp"
#include "Reflect.hpp"

#include <cbmp/client/Limits.hpp>
#include <cbmp/client/Log.hpp>

#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Rotator.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/World.hpp>

#include <Windows.h>

#include <cmath>
#include <format>
#include <utility>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace cbmp::client {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kSearchInterval = std::chrono::milliseconds(250);  // procura (FindFirstOf/FindAllOf) sem achar: no máximo 4/s

std::string Utf8(const StringType& s) {
    static_assert(sizeof(CharType) == sizeof(wchar_t));
    if (s.empty()) return {};
    const auto* w = reinterpret_cast<const wchar_t*>(s.data());
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "?";
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

StringType FromUtf16(const std::u16string& s) {
    static_assert(sizeof(CharType) == sizeof(char16_t));
    return StringType(reinterpret_cast<const CharType*>(s.data()), s.size());
}

std::string NameOf(UObject* o) { return o ? Utf8(o->GetName()) : std::string("null"); }
std::string ClassOf(UObject* o) {
    if (!o) return "null";
    UClass* c = o->GetClassPrivate();
    return c ? Utf8(c->GetName()) : std::string("?");
}

uint64_t KeyOf(UObject* o) {
    if (!o) return 0;
    const FWeakObjectPtr weak(o);
    return ObjectKey(weak.ObjectIndex, weak.ObjectSerialNumber);
}

// O ator está neste mundo? (cadeia de Outer: ator → nível → mundo)
bool IsInWorld(UObject* o, UWorld* world) {
    UObject* outer = o ? o->GetOuterPrivate() : nullptr;
    for (int depth = 0; outer && depth < 4; ++depth) {
        if (outer == static_cast<UObject*>(world)) return true;
        outer = outer->GetOuterPrivate();
    }
    return false;
}

FVector ToFVector(const Vec3& v) { return FVector(v.x, v.y, v.z); }
}  // namespace

Ue4ssGame::Ue4ssGame(ModConfig cfg) : m_cfg(cfg) {}

void Ue4ssGame::InfoOnce(const std::string& key, const std::string& msg) {
    if (m_once.First(key)) Log().Write(Level::Info, msg);
}

void Ue4ssGame::WarnOnce(const std::string& key, const std::string& msg) {
    if (m_once.First(key)) Log().Write(Level::Warn, msg);
}

UObject* Ue4ssGame::ObjectProp(UObject* owner, const TCHAR* name, const char* label) {
    if (!owner) return nullptr;
    const auto value = ReadObjectProp(owner, name);
    if (!value) {
        WarnOnce(std::string("prop:") + label, std::format("jogo: propriedade {} ausente em {}", label, ClassOf(owner)));
        return nullptr;
    }
    return *value;
}

UWorld* Ue4ssGame::CurrentWorld() {
    UObject* engine = m_engine.Get();
    if (!engine) {
        const auto now = Clock::now();
        if (now < m_nextEngineSearch) return nullptr;
        engine = UObjectGlobals::FindFirstOf(STR("GameEngine"));
        if (!engine) {
            m_nextEngineSearch = now + kSearchInterval;
            WarnOnce("engine", "jogo: GameEngine não encontrado (ainda)");
            return nullptr;
        }
        m_engine = engine;
        Log().Write(Level::Info, "jogo: GameEngine " + ClassOf(engine));
    }
    UObject* viewport = ObjectProp(engine, CBMP_T("GameViewport"), "GameViewport");
    return static_cast<UWorld*>(ObjectProp(viewport, CBMP_T("World"), "World"));
}

UObject* Ue4ssGame::PlayerController(UWorld* world) {
    if (!world) return nullptr;
    UObject* pc = m_controller.Get();
    if (pc && m_controllerWorld.Get() == static_cast<UObject*>(world)) return pc;
    const auto now = Clock::now();
    if (now < m_nextControllerSearch) return nullptr;
    m_nextControllerSearch = now + kSearchInterval;
    // Na troca de mundo os dois PlayerController convivem por um instante: fica o que é deste mundo.
    std::vector<UObject*> all;
    UObjectGlobals::FindAllOf(STR("PlayerController"), all);
    for (UObject* candidate : all) {
        if (candidate && IsInWorld(candidate, world)) {
            m_controller = candidate;
            m_controllerWorld = static_cast<UObject*>(world);
            return candidate;
        }
    }
    return nullptr;
}

UObject* Ue4ssGame::LocalPawn(UWorld* world) {
    UObject* pc = PlayerController(world);
    if (!pc) return nullptr;
    UObject* pawn = ObjectProp(pc, CBMP_T("Pawn"), "Pawn");
    if (!pawn) pawn = ObjectProp(pc, CBMP_T("AcknowledgedPawn"), "AcknowledgedPawn");
    return pawn;
}

UObject* Ue4ssGame::UiManager() {
    if (UObject* ui = m_uiManager.Get()) return ui;
    UObject* cdo = DefaultObjectOf(CBMP_T("UIManager"));
    if (!cdo) {
        WarnOnce("UIManager", "jogo: classe UIManager não encontrada; frontendHint = false e sem status na tela");
        return nullptr;
    }
    const auto ui = CallReturningObject(cdo, CBMP_T("Get"));
    if (!ui) {
        WarnOnce("UIManager.Get", "jogo: UIManager.Get não encontrada; frontendHint = false");
        return nullptr;
    }
    if (*ui) {
        m_uiManager = *ui;
        Log().Write(Level::Info, "jogo: UIManager " + NameOf(*ui));
    }
    return *ui;
}

bool Ue4ssGame::FrontendHint() {
    UObject* ui = UiManager();
    if (!ui) return false;
    const auto inFrontend = CallReturningBool(ui, CBMP_T("IsInFrontendLevel"));
    if (!inFrontend) {
        WarnOnce("IsInFrontendLevel", "jogo: UIManager.IsInFrontendLevel não encontrada; frontendHint = false");
        return false;
    }
    return *inFrontend;
}

WorldSample Ue4ssGame::SampleWorld() {
    return SafeCall("SampleWorld", [&] {
        WorldSample s{};
        UWorld* world = CurrentWorld();
        if (!world) return s;
        s.worldId = KeyOf(world);
        s.worldName = NameOf(world);
        UObject* pawn = LocalPawn(world);
        s.hasPawn = pawn != nullptr;
        s.pawnId = KeyOf(pawn);
        s.frontendHint = FrontendHint();
        if (s.pawnId != m_lastPawnKey) {
            m_lastPawnKey = s.pawnId;
            Log().Write(Level::Info, std::format("jogo: pawn local trocou (frontend={}) | {}", s.frontendHint,
                                                 DiagnoseUnsafe()));
        }
        return s;
    });
}

std::optional<LocalState> Ue4ssGame::ReadLocal() {
    return SafeCall("ReadLocal", [&]() -> std::optional<LocalState> {
        UObject* pawn = LocalPawn(CurrentWorld());
        if (!pawn) return std::nullopt;
        UObject* root = ObjectProp(pawn, CBMP_T("RootComponent"), "RootComponent");
        if (!root) return std::nullopt;
        const auto loc = ReadVectorProp(root, CBMP_T("RelativeLocation"));
        const auto rot = ReadRotatorProp(root, CBMP_T("RelativeRotation"));
        if (!loc || !rot) {
            WarnOnce("RelativeLocation", "jogo: RelativeLocation/RelativeRotation ausentes no RootComponent");
            return std::nullopt;
        }
        LocalState st{};
        st.pos = {loc->X(), loc->Y(), loc->Z()};
        st.yawDeg = rot->GetYaw();
        st.pitchDeg = rot->GetPitch();
        st.vel = {};
        st.mode = MoveMode::Other;
        if (UObject* move = ObjectProp(pawn, CBMP_T("CharacterMovement"), "CharacterMovement")) {
            if (const auto vel = ReadVectorProp(move, CBMP_T("Velocity"))) {
                st.vel = {vel->X(), vel->Y(), vel->Z()};
            } else {
                WarnOnce("Velocity", "jogo: CharacterMovement sem Velocity; velocidade 0");
            }
            if (const auto mode = ReadByteProp(move, CBMP_T("MovementMode"))) {
                st.mode = MoveModeFromMovementMode(*mode);
            } else {
                WarnOnce("MovementMode", "jogo: CharacterMovement sem MovementMode; modo Other");
            }
        }
        return st;
    });
}

bool Ue4ssGame::TeleportLocal(const Vec3& pos) {
    return SafeCall("TeleportLocal", [&] {
        UObject* pawn = LocalPawn(CurrentWorld());
        if (!pawn) return false;
        const FVector dest = ToFVector(pos);
        FRotator rot{};
        if (UObject* root = ObjectProp(pawn, CBMP_T("RootComponent"), "RootComponent")) {
            if (const auto current = ReadRotatorProp(root, CBMP_T("RelativeRotation"))) rot = *current;
        }
        bool moved = false;
        const auto used = m_teleport.Run([&](size_t step) {
            std::optional<bool> r;
            if (step == 0) {
                r = CallReturningBool(pawn, CBMP_T("K2_TeleportTo"),
                                      {{CBMP_T("DestLocation"), dest}, {CBMP_T("DestRotation"), rot}});
            } else {
                r = CallReturningBool(pawn, CBMP_T("K2_SetActorLocation"),
                                      {{CBMP_T("NewLocation"), dest}, {CBMP_T("bSweep"), false}, {CBMP_T("bTeleport"), true}});
            }
            if (!r) return Attempt::Absent;
            moved = *r;
            return Attempt::Done;
        });
        if (!used) {
            WarnOnce("teleport", "jogo: teleporte local indisponível (K2_TeleportTo e K2_SetActorLocation ausentes)");
            return false;
        }
        InfoOnce("teleport-via", std::string("jogo: teleporte local via ") +
                                     (*used == 0 ? "K2_TeleportTo" : "K2_SetActorLocation"));
        return moved;
    });
}

UObject* Ue4ssGame::Component(UObject* actor, const TCHAR* className, const char* label) {
    UClass* cls = FindClassByName(className);
    if (!cls) {
        WarnOnce(std::string("class:") + label, std::string("jogo: classe ") + label + " não encontrada");
        return nullptr;
    }
    const auto comp = CallReturningObject(actor, CBMP_T("GetComponentByClass"),
                                          {{CBMP_T("ComponentClass"), static_cast<UObject*>(cls)}});
    if (!comp) {
        WarnOnce("GetComponentByClass", "jogo: Actor.GetComponentByClass não encontrada");
        return nullptr;
    }
    if (!*comp) WarnOnce(std::string("comp:") + label, std::string("boneco: sem componente ") + label);
    return *comp;
}

void Ue4ssGame::Step(const std::string& what, UObject* target, const TCHAR* function,
                     std::initializer_list<Arg> args) {
    try {
        const bool found = SafeCall(what.c_str(), [&] { return target && CallByName(target, function, args); });
        if (found) {
            InfoOnce(std::string("ok:") + what, std::string("boneco: ") + what + " ok");  // roteiro manual: confirma o nome
        } else {
            WarnOnce(what, std::string("boneco: ") + what + " indisponível");
        }
    } catch (const GameError& e) {
        WarnOnce(what, std::string("boneco: ") + what + " falhou: " + e.what());
    }
}

void Ue4ssGame::Neutralize(UObject* actor) {
    Step("SetActorEnableCollision", actor, CBMP_T("SetActorEnableCollision"),
         {{CBMP_T("bNewActorEnableCollision"), false}});
    try {
        const bool hasController = SafeCall("Controller", [&] {
            const auto ctrl = ReadObjectProp(actor, CBMP_T("Controller"));
            return ctrl && *ctrl;
        });
        if (hasController) Step("DetachFromControllerPendingDestroy", actor, CBMP_T("DetachFromControllerPendingDestroy"), {});
    } catch (const GameError& e) {
        WarnOnce("Controller", std::string("boneco: leitura do Controller falhou: ") + e.what());
    }
    Step("SetInvulnerable", actor, CBMP_T("SetInvulnerable"), {{CBMP_T("bInInvulnerable"), true}});
    try {
        UObject* stimuli = SafeCall("CognitionStimuliSourceComponent", [&] {
            return Component(actor, CBMP_T("CognitionStimuliSourceComponent"), "CognitionStimuliSourceComponent");
        });
        if (stimuli) {
            Step("SetAutoRegisterAsSource", stimuli, CBMP_T("SetAutoRegisterAsSource"), {{CBMP_T("bFlag"), false}});
            Step("UnregisterFromPerceptionSystem", stimuli, CBMP_T("UnregisterFromPerceptionSystem"), {});
        }
    } catch (const GameError& e) {
        WarnOnce("CognitionStimuliSourceComponent", std::string("boneco: percepção falhou: ") + e.what());
    }
    Step("EnablePawnInteraction", actor, CBMP_T("EnablePawnInteraction"), {{CBMP_T("bEnable"), false}});

    if (!m_cfg.extraNeutralization) return;
    const std::pair<const TCHAR*, const char*> extras[] = {
        {CBMP_T("CameraStackComponent"), "CameraStackComponent"},
        {CBMP_T("InputWatcherComponent"), "InputWatcherComponent"},
        {CBMP_T("BP_PlayerPerceptionPoint_C"), "BP_PlayerPerceptionPoint_C"},
    };
    for (const auto& [cls, label] : extras) {
        try {
            UObject* comp = SafeCall(label, [&] { return Component(actor, cls, label); });
            if (!comp) continue;
            Step(std::string(label) + ".Deactivate", comp, CBMP_T("Deactivate"), {});
            Step(std::string(label) + ".SetComponentTickEnabled", comp, CBMP_T("SetComponentTickEnabled"),
                 {{CBMP_T("bEnabled"), false}});
        } catch (const GameError& e) {
            WarnOnce(label, std::string("boneco: ") + label + " falhou: " + e.what());
        }
    }
    // Priority fica zerado (primeiro valor do enum do jogo).
    Step("StartSettingTargetableByAiState", actor, CBMP_T("StartSettingTargetableByAiState"),
         {{CBMP_T("bTargetable"), false}, {CBMP_T("Provider"), actor}});
}

std::optional<PuppetId> Ue4ssGame::SpawnPuppet(const PuppetPose& at) {
    return SafeCall("SpawnPuppet", [&]() -> std::optional<PuppetId> {
        UWorld* world = CurrentWorld();
        UObject* pawn = LocalPawn(world);
        if (!world || !pawn) return std::nullopt;
        UClass* cls = pawn->GetClassPrivate();
        if (!cls) return std::nullopt;
        const FVector loc = ToFVector(at.pos);
        const FRotator rot(0.0, at.yawDeg, 0.0);
        AActor* spawned = world->SpawnActor(cls, &loc, &rot);
        if (!spawned) return std::nullopt;
        UObject* actor = spawned;
        const auto id = m_puppets.Add(FWeakObjectPtr(actor));
        if (!id) {
            (void)CallByName(actor, CBMP_T("K2_DestroyActor"), {});
            return std::nullopt;
        }
        InfoOnce("spawn-class", "jogo: bonecos da classe " + Utf8(cls->GetName()));
        Neutralize(actor);
        return id;
    });
}

void Ue4ssGame::Animate(UObject* actor, const PuppetPose& to) {
    if (m_animDisabled || m_cfg.puppetAnim == PuppetAnim::Off) return;
    try {
        SafeCall("animação", [&] {
            UObject* move = ObjectProp(actor, CBMP_T("CharacterMovement"), "CharacterMovement");
            if (!move) throw GameError("boneco sem CharacterMovement");
            if (!WriteVectorProp(move, CBMP_T("Velocity"), ToFVector(to.vel))) throw GameError("Velocity ausente");
            if (m_cfg.puppetAnim != PuppetAnim::Ambulatory) return;
            const double speed = std::hypot(to.vel.x, to.vel.y);
            const double yawRad = to.yawDeg * 3.14159265358979323846 / 180.0;
            const FVector dir = speed > 1.0 ? FVector(to.vel.x / speed, to.vel.y / speed, 0.0)
                                            : FVector(std::cos(yawRad), std::sin(yawRad), 0.0);
            if (!CallByName(actor, CBMP_T("SetWorldSpeedAndDirection"),
                            {{CBMP_T("WorldSpeed"), static_cast<float>(speed)}, {CBMP_T("WorldDirection"), dir}})) {
                throw GameError("SetWorldSpeedAndDirection ausente");
            }
            if (!CallByName(actor, CBMP_T("SetIsInIdle"), {{CBMP_T("isIdle"), speed < limits::kIdleMaxCmS}})) {
                throw GameError("SetIsInIdle ausente");
            }
        });
    } catch (const GameError& e) {
        m_animDisabled = true;
        Log().Write(Level::Warn, std::string("boneco: animação desligada: ") + e.what());
    }
}

PuppetResult Ue4ssGame::MovePuppet(PuppetId id, const PuppetPose& to) {
    return SafeCall("MovePuppet", [&] {
        FWeakObjectPtr* weak = m_puppets.Find(id);
        UObject* actor = weak ? weak->Get() : nullptr;
        if (!actor) {
            m_puppets.Remove(id);
            return PuppetResult::Gone;
        }
        const FVector loc = ToFVector(to.pos);
        const FRotator rot(0.0, to.yawDeg, 0.0);
        const auto used = m_move.Run([&](size_t step) {
            bool found = false;
            if (step == 0) {
                found = CallByName(actor, CBMP_T("K2_SetActorLocationAndRotation"),
                                   {{CBMP_T("NewLocation"), loc}, {CBMP_T("NewRotation"), rot},
                                    {CBMP_T("bSweep"), false}, {CBMP_T("bTeleport"), true}});
            } else if (step == 1) {
                found = CallByName(actor, CBMP_T("K2_TeleportTo"),
                                   {{CBMP_T("DestLocation"), loc}, {CBMP_T("DestRotation"), rot}});
            } else {
                UObject* root = ObjectProp(actor, CBMP_T("RootComponent"), "RootComponent");
                if (!root) throw GameError("boneco sem RootComponent");
                found = CallByName(root, CBMP_T("K2_SetWorldLocationAndRotation"),
                                   {{CBMP_T("NewLocation"), loc}, {CBMP_T("NewRotation"), rot},
                                    {CBMP_T("bSweep"), false}, {CBMP_T("bTeleport"), true}});
            }
            return found ? Attempt::Done : Attempt::Absent;
        });
        if (!used) {
            WarnOnce("move", "jogo: nenhuma função para mover bonecos (K2_SetActorLocationAndRotation, K2_TeleportTo, "
                             "K2_SetWorldLocationAndRotation)");
            return PuppetResult::Failed;
        }
        static constexpr const char* kMoveNames[] = {"K2_SetActorLocationAndRotation", "K2_TeleportTo",
                                                     "RootComponent.K2_SetWorldLocationAndRotation"};
        InfoOnce("move-via", std::string("jogo: bonecos movidos via ") + kMoveNames[*used]);
        Animate(actor, to);
        return PuppetResult::Ok;
    });
}

void Ue4ssGame::DestroyPuppet(PuppetId id) {
    SafeCall("DestroyPuppet", [&] {
        FWeakObjectPtr* weak = m_puppets.Find(id);
        if (!weak) return;
        UObject* actor = weak->Get();
        m_puppets.Remove(id);
        if (!actor) return;
        if (!CallByName(actor, CBMP_T("K2_DestroyActor"), {})) throw GameError("K2_DestroyActor não encontrada");
    });
}

void Ue4ssGame::ForgetAllPuppets() {
    SafeCall("ForgetAllPuppets", [&] { m_puppets.Clear(); });
}

bool Ue4ssGame::PlayerSingletonIntact() {
    return SafeCall("PlayerSingletonIntact", [&] {
        UObject* cdo = DefaultObjectOf(CBMP_T("Biped_Player"));
        if (!cdo) {
            WarnOnce("Biped_Player", "jogo: classe Biped_Player não encontrada; conferência do singleton desligada");
            return true;
        }
        const auto singleton = CallReturningObject(cdo, CBMP_T("GetTheBipedPlayer"));
        if (!singleton) {
            WarnOnce("GetTheBipedPlayer", "jogo: Biped_Player.GetTheBipedPlayer não encontrada; conferência desligada");
            return true;
        }
        UObject* pawn = LocalPawn(CurrentWorld());
        if (!pawn) return true;
        return *singleton == pawn;
    });
}

bool Ue4ssGame::ShowStatus(std::string_view utf8) {
    return SafeCall("ShowStatus", [&] {
        UObject* cdo = DefaultObjectOf(CBMP_T("UIManager"));
        if (!cdo) return false;
        UFunction* fn = FindFunction(cdo, CBMP_T("AddTextTickerNotification"));
        if (!fn) {
            WarnOnce("AddTextTickerNotification", "jogo: UIManager.AddTextTickerNotification não encontrada");
            return false;
        }
        const auto param = FirstStringParam(fn);
        if (!param) {
            WarnOnce("AddTextTickerNotification:param", "jogo: AddTextTickerNotification sem parâmetro FString");
            return false;
        }
        const StringType text = FromUtf16(Utf8ToUtf16(utf8));
        CallByName(cdo, CBMP_T("AddTextTickerNotification"), {{FromCharTypePtr<TCHAR>(param->c_str()), text}});
        InfoOnce("ticker", "jogo: status na tela via UIManager.AddTextTickerNotification(" + Utf8(*param) + ")");
        return true;
    });
}

std::string Ue4ssGame::DiagnoseUnsafe() {
    UWorld* world = CurrentWorld();
    UObject* pc = PlayerController(world);
    UObject* pawn = pc ? ReadObjectProp(pc, CBMP_T("Pawn")).value_or(nullptr) : nullptr;
    UObject* ack = pc ? ReadObjectProp(pc, CBMP_T("AcknowledgedPawn")).value_or(nullptr) : nullptr;
    return std::format("mundo: {} | PlayerController: {} ({}) | Pawn: {} ({}) | AcknowledgedPawn: {}", NameOf(world),
                       NameOf(pc), ClassOf(pc), NameOf(pawn), ClassOf(pawn), NameOf(ack));
}

std::string Ue4ssGame::Diagnose() {
    return SafeCall("Diagnose", [&] { return DiagnoseUnsafe(); });
}
}  // namespace cbmp::client
